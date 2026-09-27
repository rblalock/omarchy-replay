#!/usr/bin/env python3
"""Bounded shell-plugin bridge. Status is read-only; setup and capture are explicit."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ID = 'io.github.rblalock.omarchy-replay'
LIMIT = 128 * 1024


def xdg(name, fallback):
    value = os.environ.get(name, '')
    return Path(value) if value and Path(value).is_absolute() else Path(fallback)


def read_object(path, private=False):
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise RuntimeError('Replay metadata must be an owned regular file.')
        if private and info.st_mode & 0o077:
            raise RuntimeError('Replay recording state is not private.')
        content = stream.read(LIMIT + 1)
    if len(content) > LIMIT:
        raise RuntimeError('Replay metadata exceeds its size limit.')
    value = json.loads(content)
    if not isinstance(value, dict):
        raise RuntimeError('Replay metadata is invalid.')
    return value


def installed_runtime():
    app = xdg('XDG_DATA_HOME', Path.home() / '.local/share') / 'omarchy-replay/app'
    current = app / 'current'
    if not current.is_symlink():
        if current.exists():
            raise RuntimeError('Replay runtime pointer is invalid; run setup again.')
        return None, {}
    destination = current.resolve(strict=True)
    versions = app / 'versions'
    if versions.is_symlink() or app.is_symlink() or destination.parent != versions.resolve():
        raise RuntimeError('Replay runtime is outside its installation directory.')
    if destination.stat().st_uid != os.getuid():
        raise RuntimeError('Replay runtime is not owned by this user.')
    manifest = read_object(destination / 'runtime-manifest.json')
    if not (destination / 'bin/replay').is_file():
        raise RuntimeError('Replay runtime is incomplete; run setup again.')
    return destination, manifest


def compact_status():
    base = {'installed': False, 'version': '', 'update_available': False,
            'service_running': False, 'intent': 'stopped', 'state': 'setup',
            'reason': 'Set up the native app to use Replay.'}
    runtime, manifest = installed_runtime()
    if runtime is None:
        return base
    base.update(installed=True, version=str(manifest.get('version', 'unknown'))[:64],
                state='offline', reason='Recording service is offline.')
    plugin_manifest = ROOT / 'manifest.json'
    if plugin_manifest.is_file():
        source = read_object(plugin_manifest)
        base['update_available'] = source.get('version') != manifest.get('version')
    state_path = xdg('XDG_STATE_HOME', Path.home() / '.local/state') / 'omarchy-replay/recording.json'
    try:
        saved = read_object(state_path, private=True)
        if saved.get('intent') in ('running', 'paused', 'stopped'):
            base['intent'] = saved['intent']
    except FileNotFoundError:
        pass
    runtime_dir = xdg('XDG_RUNTIME_DIR', Path(tempfile.gettempdir()) / f'omarchy-replay-user-{os.getuid()}') / 'omarchy-replay'
    endpoint = runtime_dir / 'control.sock'
    try:
        directory = runtime_dir.lstat()
        info = endpoint.lstat()
    except FileNotFoundError:
        return base
    if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != os.getuid() or directory.st_mode & 0o077:
        raise RuntimeError('Replay controls are not in a private runtime directory.')
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
        raise RuntimeError('Replay control endpoint is invalid.')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(.75)
        try:
            client.connect(str(endpoint))
        except (ConnectionRefusedError, FileNotFoundError):
            return base
        _, uid, _ = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != os.getuid():
            raise RuntimeError('Replay control endpoint belongs to another user.')
        client.sendall(b'{"action":"bar-status"}\n')
        response = bytearray()
        while b'\n' not in response:
            chunk = client.recv(min(4096, LIMIT + 1 - len(response)))
            if not chunk:
                raise RuntimeError('Replay status response was interrupted.')
            response.extend(chunk)
            if len(response) > LIMIT:
                raise RuntimeError('Replay status response exceeds its size limit.')
    status = json.loads(response)
    if not isinstance(status, dict) or 'error' in status:
        raise RuntimeError('Replay could not read status; update or restart the native app.')
    base.update(service_running=status.get('running') is True,
                intent=status.get('intent', 'stopped'), state=str(status.get('state', 'unknown'))[:80],
                reason=str(status.get('reason', ''))[:500])
    if base['intent'] not in ('running', 'paused', 'stopped'):
        raise RuntimeError('Replay returned invalid recording intent.')
    error = status.get('config_error') or status.get('exclusions_error')
    if error:
        base['error'] = str(error)[:500]
    return base


@contextmanager
def setup_lock(build):
    build.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if build.parent.is_symlink() or build.parent.stat().st_uid != os.getuid():
        raise RuntimeError('Replay build cache must belong to this user.')
    descriptor = os.open(build.with_suffix('.lock'), os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or info.st_mode & 0o077:
            raise RuntimeError('Replay setup lock is not a private owned file.')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('Replay setup is already running; use its existing terminal.') from error
        yield
    finally:
        os.close(descriptor)


def setup_run():
    source = read_object(ROOT / 'manifest.json')
    if source.get('id') != PLUGIN_ID or not (ROOT / 'CMakeLists.txt').is_file():
        raise RuntimeError('Native setup requires the Omarchy Replay plugin source folder.')
    key = hashlib.sha256(os.fsencode(ROOT)).hexdigest()[:16]
    build = xdg('XDG_CACHE_HOME', Path.home() / '.cache') / 'omarchy-replay/build' / key
    commands = [
        ['cmake', '-S', str(ROOT), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF'],
        ['cmake', '--build', str(build), '--target', 'replay', '--parallel', '2'],
        [sys.executable, '-B', str(ROOT / 'scripts/install_recording_service.py'), '--binary', str(build / 'replay')],
    ]
    with setup_lock(build):
        print('Building the native Replay app. This does not install packages or start recording on first setup.', flush=True)
        for command in commands:
            result = subprocess.run(command, check=False)
            if result.returncode:
                raise RuntimeError('Setup did not complete. Review the output above and the dependencies in the plugin README.')
    print('Replay is ready. Open it from the top bar; choose Start recording when you want to capture.', flush=True)


def action(name):
    if name == 'status':
        return compact_status()
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', OMP_THREAD_LIMIT='1')
    if name == 'setup':
        subprocess.Popen(['omarchy', 'launch', 'terminal', sys.executable, '-B', str(Path(__file__).resolve()), 'setup-run'],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env=environment, start_new_session=True)
        return {'ok': True, 'reason': 'Setup opened in a terminal.'}
    runtime, _ = installed_runtime()
    if runtime is None:
        raise RuntimeError('Set up the native Replay app first.')
    from package_runtime import verify_runtime
    verify_runtime(runtime, existing=True)
    if name in ('open', 'settings'):
        command = [str(runtime / 'scripts/replay'), 'open', '--notify-errors']
        if name == 'settings':
            command.append('--settings')
        subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env=environment, start_new_session=True)
        return {'ok': True, 'reason': 'Opening Replay settings.' if name == 'settings' else 'Opening Replay.'}
    if name not in ('start', 'stop', 'pause', 'resume'):
        raise RuntimeError('Unsupported Replay action.')
    result = subprocess.run([str(runtime / 'bin/replay'), 'daemon', name], capture_output=True,
                            timeout=15, env=environment, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-500:] or 'Replay could not apply the recording action.')
    return compact_status()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('status', 'open', 'settings', 'start', 'stop', 'pause', 'resume', 'setup', 'setup-run'))
    args = parser.parse_args(argv)
    if args.action == 'setup-run':
        try:
            setup_run()
            return 0
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            print(f'\nReplay setup: {error}', file=sys.stderr, flush=True)
            if sys.stdin.isatty():
                input('\nPress Enter to close. ')
            return 1
    try:
        print(json.dumps(action(args.action)))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)[-500:], 'reason': str(error)[-500:]}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
