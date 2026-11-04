#!/usr/bin/env python3
"""Finite synthetic service ownership, recovery and resource checks; never capture a desktop."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

BINARY = str(Path(sys.argv[1]).resolve())
REPORT = Path(sys.argv[2]) if len(sys.argv) > 2 else None
ENV = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='',
           QT_STYLE_OVERRIDE='Fusion', OMP_THREAD_LIMIT='1', WAYLAND_DISPLAY='/nonexistent/replay-service-test')
POLICY = ['--scheduler', 'fixed', '--ocr-cpu-percent', '20', '--ocr-mode', 'incremental', '--ocr-max-wall-ms', '60000']


def command(*args, okay=True):
    result = subprocess.run([BINARY, *map(str, args)], env=ENV, capture_output=True, text=True, timeout=15)
    if okay:
        assert result.returncode == 0, f'{args[:2]}: {result.stderr}'
        return json.loads(result.stdout)
    return result


def service(dataset, action, *args):
    return command('service', action, '--dir', dataset, *args)


def wait_for(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('Expected service state did not arrive before its finite deadline')


def seed(path):
    command('demo', '--dir', path, '--frames', 12, '--width', 640, '--height', 360,
            '--codec', 'webp', '--indexing', 'deferred', '--ocr-cpu-percent', 1,
            '--ocr-max-wall-ms', 60000, '--pending-frames', 0, '--pending-mib', 8,
            '--max-mib', 64, '--drain-seconds', 0)
    assert command('status', '--dir', path)['pending'] > 0


def process_sample(pid):
    root = Path('/proc') / str(pid)
    fields = (root / 'stat').read_text().rsplit(')', 1)[1].split()
    memory = dict(line.split(':', 1) for line in (root / 'smaps_rollup').read_text().splitlines()[1:])
    io = dict(line.split(':', 1) for line in (root / 'io').read_text().splitlines())
    return {'cpu_seconds': (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK'),
            'pss_mib': int(memory['Pss'].split()[0]) / 1024,
            'read_bytes': int(io['read_bytes']), 'write_bytes': int(io['write_bytes']),
            'read_characters': int(io['rchar']), 'written_characters': int(io['wchar'])}


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except FileNotFoundError:
        return False


def main():
    report = {'data_origin': 'synthetic', 'checks': []}
    datasets = []
    with tempfile.TemporaryDirectory(prefix='replay-service-') as temporary, \
         tempfile.TemporaryDirectory(prefix='replay-service-runtime-', dir='/dev/shm') as runtime:
        ENV['XDG_RUNTIME_DIR'] = runtime
        root = Path(temporary)
        try:
            dataset = root / 'lifecycle'
            seed(dataset); datasets.append(dataset)
            unknown = service(dataset, 'status')
            assert not unknown['configured'] and not unknown['running']
            rejected = command('service', 'start', '--dir', dataset, okay=False)
            assert rejected.returncode and 'No saved indexing policy' in rejected.stderr
            paused = service(dataset, 'pause', *POLICY)
            assert paused['paused'] and paused['enabled'] and not paused['running']
            started = service(dataset, 'start')
            assert started['running'] and started['paused'] and not started['worker_running']
            pid = started['service_pid']
            durable = dataset / '.index-service.json'
            assert durable.stat().st_mode & 0o077 == 0
            durable_before = (durable.read_bytes(), durable.stat().st_mtime_ns)
            time.sleep(2.2)
            assert (durable.read_bytes(), durable.stat().st_mtime_ns) == durable_before, 'Heartbeat rewrote persistent settings'
            still_paused = service(dataset, 'ensure')  # What reopening a saved trial requests.
            assert still_paused['paused'] and still_paused['service_pid'] == pid
            still_paused = service(dataset, 'ensure', '--scheduler', 'fixed', '--ocr-cpu-percent', 99)
            assert still_paused['paused'] and still_paused['policy']['ocr_cpu_percent'] == 20, \
                'Automatic ensure overwrote a later saved policy with stale supplied defaults'
            report['checks'].append('start and reopen preserve a saved pause without OCR')

            viewer = subprocess.Popen([BINARY, 'view', '--dir', str(dataset)], env=ENV,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                service(dataset, 'resume')
                active = wait_for(lambda: (s if (s := service(dataset, 'status'))['worker_running'] else None))
                worker_pid = active['worker_pid']
                competing = command('index', '--dir', dataset, '--ocr-cpu-percent', 99, okay=False)
                assert competing.returncode, 'Competing worker unexpectedly acquired its lock'
                after_competitor = service(dataset, 'status')
                if after_competitor['worker_running']:
                    assert after_competitor.get('worker_policy', {}).get('pid') != 0
                    assert after_competitor.get('effective_cpu_percent') != 99, 'Rejected worker replaced live policy'
                viewer.terminate(); viewer.wait(timeout=3)
                after_close = service(dataset, 'status')
                assert after_close['running'] and after_close['service_pid'] == pid
                wait_for(lambda: command('status', '--dir', dataset)['pending'] == 0, 30)
                wait_for(lambda: not service(dataset, 'status')['worker_running'])
                assert not alive(worker_pid), 'Caught-up service retained its heavy OCR child'
                report['checks'].append('indexing survives viewer close, drains and releases OCR memory')
            finally:
                if viewer.poll() is None:
                    viewer.terminate(); viewer.wait(timeout=3)

            # Releasing the OCR lease can precede QProcess reaping. Linux adds
            # a reaped child's I/O counters to its parent, so wait for a fresh
            # supervisor receipt after that accounting boundary before sampling.
            wait_for(lambda: (s if (s := service(dataset, 'status'))['running']
                              and not s['worker_running'] and s.get('worker_pid') == 0
                              and s.get('progress', {}).get('pending') == 0
                              and 0 <= s['heartbeat_age_ms'] < 1000 else None))
            before = process_sample(pid)
            measured_at = time.monotonic()
            time.sleep(4)
            after = process_sample(pid)
            elapsed = time.monotonic() - measured_at
            report['idle_supervisor'] = {
                'seconds': round(elapsed, 3), 'pss_mib': round(after['pss_mib'], 3),
                'percent_one_cpu': round((after['cpu_seconds'] - before['cpu_seconds']) / elapsed * 100, 3),
                **{key: after[key] - before[key] for key in ('read_bytes', 'write_bytes', 'read_characters', 'written_characters')},
                'scope': 'Supervisor only, after synthetic queue drained; four seconds, warm cache, runtime on tmpfs'}
            heartbeat = service(dataset, 'status')
            assert heartbeat['running'] and heartbeat['heartbeat_age_ms'] < 5000
            stopped = service(dataset, 'stop')
            assert not stopped['running'], 'Stop returned before supervisor lease release'
            wait_for(lambda: not alive(pid), 3)
            stopped_bytes = durable.read_bytes()
            ensured = service(dataset, 'ensure')
            assert not ensured['running'] and not ensured['enabled'] and durable.read_bytes() == stopped_bytes, \
                'Automatic ensure undid a saved Stop or rewrote its intent'
            # Model Stop winning the control lease while an automatic ensure
            # waits. The latter must read the new intent after it acquires it.
            control_path = Path(runtime) / 'replay' / hashlib.sha256(os.fsencode(dataset.resolve())).hexdigest() / 'control.lock'
            with control_path.open('a+b') as control:
                fcntl.flock(control, fcntl.LOCK_EX)
                intent = json.loads(stopped_bytes); intent['enabled'] = True
                durable.write_text(json.dumps(intent))
                automatic = subprocess.Popen([BINARY, 'service', 'ensure', '--dir', str(dataset)], env=ENV,
                                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    time.sleep(.15)
                    assert automatic.poll() is None, 'Automatic ensure bypassed the shared control lease'
                    durable.write_bytes(stopped_bytes)
                    fcntl.flock(control, fcntl.LOCK_UN)
                    stdout, stderr = automatic.communicate(timeout=4)
                    assert automatic.returncode == 0, stderr
                    ordered = json.loads(stdout)
                    assert not ordered['running'] and not ordered['enabled'], 'Queued ensure undid the newer Stop'
                finally:
                    if automatic.poll() is None:
                        automatic.terminate(); automatic.wait(timeout=3)
            report['checks'].append('atomic automatic ensure preserves Stop, including control-lease ordering')
            resumed = service(dataset, 'resume')
            assert resumed['running'] and resumed['service_pid'] != pid, 'Immediate stop/resume lost the supervisor'
            service(dataset, 'pause')
            service(dataset, 'stop')
            restarted = service(dataset, 'start')
            assert restarted['running'] and restarted['paused'], 'Pause preference did not survive service restart'
            service(dataset, 'stop')
            report['checks'].append('bounded stop, immediate resume and persistent pause')

            external = root / 'external'
            seed(external); datasets.append(external)
            with (external / '.indexer.lock').open('a+b') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                service(external, 'start', *POLICY)
                status = wait_for(lambda: (s if (s := service(external, 'status'))['external_worker'] else None))
                assert status['worker_pid'] == 0 and status['state'] == 'waiting'
                service(external, 'pause')
                assert service(external, 'status')['worker_running'], 'Service pause interfered with an external owner'
                service(external, 'stop')
                assert command('status', '--dir', external)['indexer_running'], 'Service stop released another owner lock'
                fcntl.flock(lock, fcntl.LOCK_UN)
            report['checks'].append('external worker ownership is respected')

            broken = root / 'broken-model'
            seed(broken); datasets.append(broken)
            model = root / 'broken-model-files'; model.mkdir()
            model_file = model / 'eng.traineddata'; model_file.write_bytes(b'not a trained model')
            service(broken, 'start', *POLICY, '--ocr-data-path', model)
            # An unloadable model stops the service on the first worker exit,
            # names the model problem and keeps every moment pending.
            error = wait_for(lambda: (s if (s := service(broken, 'status'))['state'] == 'error' else None), 15)
            assert 'OCR model' in error['recovery'] and str(model) in error['error'], error
            assert len(error['log_tail']) <= 8192
            progress = command('status', '--dir', broken)
            assert progress['failed'] == 0 and progress['pending'] > 0, 'Unloadable model failed retained moments'
            time.sleep(2.2)
            assert command('status', '--dir', broken)['pending'] == progress['pending'] and not service(broken, 'status')['worker_running'], \
                'Unloadable model was retried without a settings change'
            model_file.unlink()
            invalid = service(broken, 'status')
            assert invalid['state'] == 'error' and invalid['policy_error'] and 'enabled' in invalid
            assert service(broken, 'pause')['paused'], 'Invalid model blocked pause'
            assert not service(broken, 'stop')['enabled'], 'Invalid model blocked stop'
            recovered = service(broken, 'start', *POLICY)
            assert recovered['paused'] and recovered['configured'], 'Explicit valid replacement policy did not recover'
            service(broken, 'stop')
            service(broken, 'resume')
            wait_for(lambda: command('status', '--dir', broken)['pending'] == 0, 30)
            service(broken, 'stop')
            report['checks'].append('model failure retries are bounded; invalid-model status/control/recovery remain usable')

            crashes = root / 'crash-retries'
            seed(crashes); datasets.append(crashes)
            service(crashes, 'start', '--scheduler', 'fixed', '--ocr-cpu-percent', 1,
                    '--ocr-mode', 'incremental', '--ocr-max-wall-ms', 60000)
            for _ in range(3):
                active = wait_for(lambda: (s if (s := service(crashes, 'status')).get('worker_running')
                                          and s.get('worker_pid', 0) > 0 else None), 18)
                owned_worker = active['worker_pid']
                # Kill only the worker identified by this isolated synthetic service.
                argv = Path(f'/proc/{owned_worker}/cmdline').read_bytes().split(b'\0')
                assert b'index' in argv and os.fsencode(crashes) in argv
                os.kill(owned_worker, signal.SIGKILL)
                wait_for(lambda: not alive(owned_worker), 3)
            error = wait_for(lambda: (s if (s := service(crashes, 'status'))['state'] == 'error' and not s['running'] else None), 5)
            assert 'three times' in error['error'], error
            report['checks'].append('three unexpected owned-worker exits stop bounded retries')
        finally:
            for dataset in datasets:
                result = command('service', 'stop', '--dir', dataset, okay=False)
                assert result.returncode == 0, f'Cannot clean up owned test service: {result.stderr}'
    if REPORT:
        REPORT.parent.mkdir(parents=True, exist_ok=True)
        REPORT.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
