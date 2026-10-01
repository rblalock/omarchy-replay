#!/usr/bin/env python3
"""Shared recorder lifecycle against fictional pixels and injected desktop signals."""
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

BINARY = str(Path(sys.argv[1]).resolve())


def eventually(check, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = check()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('condition did not settle within its deadline')


def main():
    with tempfile.TemporaryDirectory(prefix='replay-recording-service-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='', QT_STYLE_OVERRIDE='Fusion', OMP_THREAD_LIMIT='1')
        for kind in ('CONFIG', 'DATA', 'STATE', 'CACHE', 'RUNTIME'):
            path = root / kind.lower(); path.mkdir(mode=0o700)
            env['XDG_' + kind + ('_DIR' if kind == 'RUNTIME' else '_HOME')] = str(path)
        config = root / 'config/omarchy-replay/config.toml'; config.parent.mkdir()
        valid_config = ('[recording]\noutput="TEST-1"\ninterval_seconds=0.5\n'
                        '[storage]\nretention_days=1\nmax_disk_mib=64\nmin_free_mib=0\n'
                        '[indexing]\ncpu_ceiling_percent=0\n'
                        '[exclusions]\napps=[]\n')
        config.write_text(valid_config)
        history = root / 'data/omarchy-replay/history'
        # Detection is read-only. This executable must never run, and all
        # meeting content in this test is fictional and locally generated.
        tools = root / 'bin'; tools.mkdir()
        recorder_stub = tools / 'omarchy-meeting-recorder'
        recorder_marker = root / 'recorder-was-launched'
        recorder_stub.write_text('#!/bin/sh\n: > "' + str(recorder_marker) + '"\n')
        recorder_stub.chmod(0o700)
        # Isolate dependency discovery even on hosts with the real recorder
        # installed; removing the fixture must not find a fallback executable.
        for command in ('python3', 'systemctl', 'systemd-run', 'hyprctl', 'ffmpeg', 'ffprobe', 'tesseract'):
            resolved = shutil.which(command)
            if resolved:
                (tools / command).symlink_to(resolved)
        env['PATH'] = str(tools)
        meetings = root / 'meetings'; meetings.mkdir(mode=0o700)
        meeting_config = valid_config + '[meetings]\nenabled=true\ndirectory=' + json.dumps(str(meetings)) + '\n'

        def meeting_fixture(name):
            folder = meetings / name; folder.mkdir(mode=0o700)
            transcript = folder / 'transcript.md'
            transcript.write_text('# Fictional planning meeting\n\n[00:00] Discuss the synthetic paper invoice.\n')
            manifest = folder / (name + '.meeting-recorder')
            manifest.write_text(json.dumps(dict(title='Synthetic ' + name, started_at=int(time.time()) - 10, duration_secs=5)))
            for file in (transcript, manifest):
                os.utime(file, (time.time() - 5, time.time() - 5))
            return folder

        def meeting_count():
            with sqlite3.connect(history / 'index.sqlite', timeout=2) as db:
                if not db.execute("SELECT 1 FROM sqlite_master WHERE name='meeting_records'").fetchone():
                    return 0
                return db.execute('SELECT count(*) FROM meeting_records').fetchone()[0]

        meeting_fixture('first')
        sensor = root / 'environment.json'
        monitor = dict(id=1, name='TEST-1', make='Synthetic', model='Fixture', serial='TEST-1', description='Synthetic display',
                       width=1920, height=1080, scale=1.0, transform=0, x=0, y=0, disabled=False, dpmsStatus=True, mirrorOf='none')
        generation = 0
        screensaver = dict(address='0x1234', **{'class': 'org.omarchy.screensaver'},
                           initialClass='org.omarchy.screensaver', title='Synthetic screensaver',
                           mapped=True, hidden=False, visible=True, at=[0, 0], size=[1920, 1080], monitor=1)

        def desktop(**fields):
            nonlocal generation
            generation += 1
            value = dict(known=True, generation=generation, locked=False, sleeping=False, monitors=[monitor], windows=[])
            value.update(fields)
            temp = sensor.with_suffix('.tmp'); temp.write_text(json.dumps(value)); temp.replace(sensor)

        def call(action, *args, success=True):
            result = subprocess.run([BINARY, 'daemon', action, *args], env=env, capture_output=True, text=True, timeout=15)
            if success:
                assert result.returncode == 0, result.stderr
                return json.loads(result.stdout)
            assert result.returncode != 0, 'invalid/destructive action was accepted'

        def count():
            with sqlite3.connect(history / 'index.sqlite', timeout=2) as db:
                return db.execute('SELECT COUNT(*) FROM observations').fetchone()[0]

        def status():
            return call('status')

        desktop()
        paths = call('paths')
        assert paths['history'] == str(history) and paths['default_history'] == str(history)
        assert paths['state'] == str(root / 'state/omarchy-replay')
        assert not (history / 'index.sqlite').exists(), 'paths initialized an archive'
        config.write_text('[recording]\ninterval_seconds="invalid"\n')
        call('paths', success=False)
        assert not (history / 'index.sqlite').exists(), 'invalid paths selected a new archive'
        assert not (root / 'state/omarchy-replay').exists(), 'paths wrote fallback state'
        config.write_text(valid_config)
        unrelated = root / 'unrelated-folder'; unrelated.mkdir()
        (unrelated / 'keep.txt').write_text('Keep this unrelated folder intact.')
        config.write_text(valid_config.replace('[storage]\n', '[storage]\ndirectory=' + json.dumps(str(unrelated)) + '\n'))
        call('init', success=False)
        assert [entry.name for entry in unrelated.iterdir()] == ['keep.txt'], 'init modified an unrelated folder'
        alias = root / 'symlink-history'; alias.symlink_to(unrelated, target_is_directory=True)
        config.write_text(valid_config.replace('[storage]\n', '[storage]\ndirectory=' + json.dumps(str(alias)) + '\n'))
        call('init', success=False)
        config.write_text(valid_config)
        initialized = call('init')
        assert initialized['recording_started'] is False and count() == 0
        assert status()['running'] is False, 'initialization started recording service'
        child = None
        logs = (root / 'coordinator.log').open('w+')

        def launch():
            nonlocal child
            child = subprocess.Popen([BINARY, 'daemon', 'run', '--synthetic', '--synthetic-environment', str(sensor)],
                                     env=env, stdout=logs, stderr=logs)
            eventually(lambda: child.poll() is None and status().get('running'))

        try:
            launch()
            assert status()['intent'] == 'stopped' and count() == 0
            assert status()['meetings']['available'] and not status()['meetings']['enabled']
            assert meeting_count() == 0 and not status()['meetings']['worker_running']
            free_mib = os.statvfs(root).f_bavail * os.statvfs(root).f_frsize // (1024 * 1024)
            if free_mib < 1044480:
                blocked = meeting_config.replace('min_free_mib=0', 'min_free_mib=' + str(free_mib + 4096))
                config.write_text(blocked); call('reload')
                eventually(lambda: status()['meetings'].get('limited') and not status()['meetings'].get('syncing'))
                assert status()['meetings']['error'], 'limited meeting import was reported without an explanation'
                assert meeting_count() == 0 and count() == 0, 'meeting import ignored free-disk reserve'
            config.write_text(meeting_config); call('reload')
            eventually(lambda: meeting_count() == 1)
            assert status()['intent'] == 'stopped' and count() == 0, 'meeting opt-in started screen capture'
            eventually(lambda: status()['meetings'].get('count') == 1)
            call('meeting-sync', '--history', str(history), '--source', str(meetings),
                 '--retention-days', '1', '--max-disk-mib', '64', '--min-free-mib', '0', success=False)
            call('index-pause')
            assert not status()['meetings']['worker_running']
            meeting_fixture('second')
            time.sleep(1.2)
            assert meeting_count() == 1, 'paused indexing imported a new meeting'
            call('index-resume')
            eventually(lambda: meeting_count() == 2)
            config.write_text(meeting_config.replace('enabled=true', 'enabled=false')); call('reload')
            assert not status()['meetings']['worker_running'] and meeting_count() == 2
            meeting_fixture('third')
            time.sleep(1.2)
            assert meeting_count() == 2, 'disabled integration imported a new meeting'
            # Missing optional dependency leaves valid recording configuration
            # and retained imports intact; it cannot silently opt back in.
            recorder_stub.unlink()
            config.write_text(meeting_config); call('reload')
            eventually(lambda: not status()['meetings']['available'])
            assert not status()['meetings']['worker_running'] and not status()['config_error']
            assert meeting_count() == 2 and not recorder_marker.exists()
            config.write_text(valid_config); call('reload')
            assert not status()['exclusions_pending'] and not status()['exclusions_error'], 'synthetic service attempted compositor mask installation'
            # Exclusion edits are accepted while stopped or paused without
            # activating recording/indexing or invoking a native installer.
            call('index-pause')
            for capture_intent in ('stopped', 'paused'):
                if capture_intent == 'paused':
                    call('pause')
                config.write_text(valid_config.replace('apps=[]', 'apps=["fixture.private"]\nskip_apps=["mpv"]'))
                edited = call('reload')
                time.sleep(.15)
                settled = status()
                assert edited['intent'] == settled['intent'] == capture_intent and settled['indexing_paused']
                assert not settled['exclusions_pending'] and not settled['exclusions_error']
                assert settled['capture_attempts'] == 0 and count() == 0
                config.write_text(valid_config); call('reload')
            call('stop'); call('index-resume')
            second = subprocess.run([BINARY, 'daemon', 'run', '--synthetic'], env=env, capture_output=True, timeout=8)
            assert second.returncode != 0 and count() == 0, 'second coordinator acquired shared history'
            # Explicit debugging is bounded and cannot start capture or leak
            # private window metadata into its numeric report.
            debug = call('debug', '--seconds', '1')
            assert not debug['active'] and debug['capture_attempts'] == 0
            assert status()['intent'] == 'stopped' and count() == 0
            call('debug', '--seconds', '0', success=False)
            abandoned_debug = subprocess.Popen([BINARY, 'daemon', 'debug', '--seconds', '1'],
                                                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                eventually(lambda: call('debug-status').get('active'))
                abandoned_debug.kill(); abandoned_debug.wait(timeout=3)
                eventually(lambda: not call('debug-status').get('active'))
                assert not call('debug-status')['environment']['enabled'], 'abandoned debug session kept monitoring'
                assert status()['intent'] == 'stopped' and count() == 0
            finally:
                if abandoned_debug.poll() is None:
                    abandoned_debug.kill(); abandoned_debug.wait(timeout=3)
            # Capture-setting changes reset forecast evidence, including across
            # restart. A storage-only edit can still project the same rate.
            config.write_text(valid_config.replace('interval_seconds=0.5', 'interval_seconds=0.75'))
            call('reload')
            state_file = root / 'state/omarchy-replay/recording.json'
            boundary = json.loads(state_file.read_text())['forecast_sample_after_ms']
            assert boundary > 0 and count() == 0
            child.terminate(); child.wait(timeout=15); launch()
            assert json.loads(state_file.read_text())['forecast_sample_after_ms'] == boundary
            assert status()['intent'] == 'stopped'
            config.write_text(valid_config.replace('interval_seconds=0.5', 'interval_seconds=0.75').replace('max_disk_mib=64', 'max_disk_mib=128'))
            call('reload')
            assert json.loads(state_file.read_text())['forecast_sample_after_ms'] == boundary
            config.write_text(valid_config); call('reload')
            desktop(unstable=True)
            call('start'); retained = count(); attempted = status()['capture_attempts']
            time.sleep(1.6)
            snapshot = status()
            assert count() == retained, 'an unstable desktop retained a screenshot'
            assert 1 <= snapshot['capture_attempts'] - attempted <= 5, 'unstable capture spun without retry pacing'
            desktop()
            eventually(lambda: count() > retained)
            call('start'); eventually(lambda: count() >= 3)
            call('pause'); retained = count(); time.sleep(1.2)
            assert count() == retained and status()['intent'] == 'paused'
            desktop(locked=True); call('resume')
            eventually(lambda: status()['state'] == 'locked')
            assert count() == retained
            desktop(); eventually(lambda: count() > retained)
            # Recording-only skips stop archive admission without treating text
            # shown inside a local meeting window as another app's identity.
            config.write_text(valid_config + 'skip_apps=["mpv"]\n'); call('reload')
            player = dict(screensaver, **{'class': 'mpv', 'initialClass': 'mpv', 'title': 'Synthetic mirror'})
            desktop(windows=[player]); eventually(lambda: status()['state'] == 'excluded_window')
            retained = count(); time.sleep(.7)
            assert count() == retained and status()['intent'] == 'running', 'recording-only app retained frames'
            for app in ('chromium', 'zoom'):
                meeting = dict(player, **{'class': app, 'initialClass': app, 'title': 'Shared screen: mpv Steam'})
                desktop(windows=[meeting]); eventually(lambda: count() > retained); retained = count()
            desktop(windows=[player]); eventually(lambda: status()['state'] == 'excluded_window')
            call('pause'); retained = count(); desktop(); time.sleep(.7)
            assert count() == retained and status()['intent'] == 'paused', 'skip close cleared manual pause'
            config.write_text(valid_config); call('reload'); call('resume')
            eventually(lambda: count() > retained)
            desktop(windows=[screensaver]); eventually(lambda: status()['state'] == 'excluded_window')
            retained = count(); time.sleep(.7)
            assert count() == retained and status()['intent'] == 'running', 'screensaver retained frames or changed intent'
            desktop(); eventually(lambda: count() > retained)
            desktop(windows=[screensaver]); eventually(lambda: status()['state'] == 'excluded_window')
            call('pause'); retained = count()
            desktop(); time.sleep(.7)
            assert count() == retained and status()['intent'] == 'paused', 'screensaver close cleared manual pause'
            call('resume'); eventually(lambda: count() > retained)
            desktop(sleeping=True); eventually(lambda: status()['state'] == 'sleeping')
            retained = count(); desktop(locked=True); eventually(lambda: status()['state'] == 'locked')
            time.sleep(.7); assert count() == retained, 'wake while locked captured a screen'
            desktop(); eventually(lambda: count() > retained)
            desktop(monitors=[]); eventually(lambda: status()['state'] == 'output_unavailable')
            retained = count(); time.sleep(.7); assert count() == retained
            desktop(); eventually(lambda: count() > retained)
            call('pause'); call('index-pause'); retained = count()
            child.terminate(); child.wait(timeout=15)
            launch(); time.sleep(.8)
            assert status()['intent'] == 'paused' and status()['indexing_paused'] and count() == retained
            # A temporary environment change must never clear a manual pause.
            desktop(locked=True); desktop(); time.sleep(.7)
            assert count() == retained
            config.write_text('[recording]\ninterval_seconds="invalid"\n')
            assert call('reload')['config_error'], 'invalid config replaced usable settings'
            child.terminate(); child.wait(timeout=15)
            launch()
            assert status()['config_error'] and status()['intent'] == 'paused', 'restart lost last valid config/intent'
            config.write_text(valid_config); call('reload')
            assert not status()['config_error']
            call('delete-recent', '--seconds', '300', success=False)
            assert count() == retained
            with sqlite3.connect(history / 'index.sqlite') as db:
                greatest_id = db.execute('SELECT MAX(id) FROM frames').fetchone()[0]
            call('delete-recent', '--seconds', '300', '--confirmed')
            eventually(lambda: count() == 0 and not status()['deleting'])
            call('resume'); eventually(lambda: count() >= 2); call('pause')
            with sqlite3.connect(history / 'index.sqlite') as db:
                assert db.execute('SELECT MIN(id) FROM frames').fetchone()[0] > greatest_id, 'moment IDs were reused'
            call('index-resume')
            eventually(lambda: status()['progress'].get('pending') == 0, 30)
            assert status()['progress'].get('failed') == 0
            # Focused mode: the service starts without a configured output and
            # records whichever display the environment selects for each tick.
            second_monitor = dict(monitor, id=2, name='TEST-2', serial='TEST-2',
                                  description='Synthetic second display', x=1920, width=1920, height=1200)

            def focus_desktop(first_focused=True, **fields):
                desktop(monitors=[dict(monitor, focused=first_focused),
                                  dict(second_monitor, focused=not first_focused)], **fields)

            call('pause')
            config.write_text(valid_config.replace('output="TEST-1"', 'display_mode="focused"')); call('reload')
            focus_desktop()
            retained = count(); call('start')
            eventually(lambda: count() > retained)
            focused = status()
            assert focused['display_mode'] == 'focused', 'status lost the configured display mode'
            assert focused['output'] == '' and focused['last_capture_target'] == 'TEST-1', 'focused mode without a configured display did not record the focused display'
            assert focused['recording_output'] == 'TEST-1'
            call('pause')
            assert status()['recording_output'] == '', 'a non-recording state kept its last capture display'
            # R4: a focus move before the next tick moves the capture target.
            config.write_text(valid_config.replace('interval_seconds=0.5', 'display_mode="focused"\ninterval_seconds=0.5')); call('reload')
            focus_desktop(); retained = count(); call('start')
            eventually(lambda: count() > retained)
            focus_desktop(False)
            eventually(lambda: status()['recording_output'] == 'TEST-2')
            assert status()['last_capture_target'] == 'TEST-2', 'the capture target did not follow focus'
            assert status()['output'] == 'TEST-1', 'a focus move changed the configured display'
            # R5: zero focused displays block capture without substitution.
            desktop(monitors=[dict(monitor, focused=False), dict(second_monitor, focused=False)])
            eventually(lambda: status()['state'] == 'focus_unknown')
            retained = count(); time.sleep(.8)
            assert count() == retained and status()['recording_output'] == '', 'focus_unknown retained a frame or kept a capture display'
            # R8: a focus move between the pre- and post-capture snapshots
            # discards the frame even without a generation change.
            desktop(alternate_focus=True, monitors=[dict(monitor, focused=True), dict(second_monitor, focused=False)])
            eventually(lambda: status()['state'] == 'desktop-changed')
            retained = count(); time.sleep(.8)
            assert count() == retained, 'a focus move between snapshots retained a frame'
            # R9: focus events never schedule captures; at most one frame is
            # retained per configured interval.
            focus_desktop(); eventually(lambda: count() > retained)
            watermark = int(time.time() * 1000); flipped = False
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                focus_desktop(flipped); flipped = not flipped; time.sleep(.2)
            time.sleep(.7)
            with sqlite3.connect(history / 'index.sqlite') as db:
                stamps = [row[0] for row in db.execute(
                    'SELECT timestamp_ms FROM frames WHERE timestamp_ms >= ? ORDER BY timestamp_ms, id', (watermark,))]
            assert stamps, 'rapid focus changes stopped retaining frames entirely'
            gaps = [later - earlier for earlier, later in zip(stamps, stamps[1:])]
            # Floor at the configured interval (0.5 s here), not a loose margin:
            # the cadence math keeps retained frames at least one interval apart,
            # so a regression retaining a frame between ticks must fail here.
            assert all(gap >= 500 for gap in gaps), 'rapid focus changes retained frames faster than the configured interval'
            # A display-mode change is a capture-setting change for the storage
            # forecast, and an explicit output selection leaves focused mode.
            call('pause')
            state_file = root / 'state/omarchy-replay/recording.json'
            boundary = json.loads(state_file.read_text())['forecast_sample_after_ms']
            config.write_text(valid_config); call('reload')
            assert json.loads(state_file.read_text())['forecast_sample_after_ms'] > boundary, 'a display-mode change did not reset the storage forecast'
            config.write_text(valid_config.replace('interval_seconds=0.5', 'display_mode="focused"\ninterval_seconds=0.5'))
            call('init', '--output', 'TEST-1')
            assert call('reload')['display_mode'] == 'fixed', 'an explicit display selection did not leave focused mode'
            config.write_text(valid_config); call('reload')
            # Switching archive folders is explicit, keeps the old archive, and
            # preserves both the recording and index control choices.
            original_history = history
            original_count = count()
            external = root / 'mounted-disk/history'; external.mkdir(parents=True)
            config.write_text(valid_config.replace('[storage]\n', '[storage]\ndirectory=' + json.dumps(str(external)) + '\n'))
            switched = call('reload'); history = external
            assert switched['history_directory'] == str(external) and switched['intent'] == 'paused'
            assert switched['storage_available'] and not switched['config_error'] and count() == 0
            assert call('paths')['history'] == str(external)
            custom_config = config.read_text()
            config.write_text('[recording]\ninterval_seconds="invalid"\n')
            state_file = root / 'state/omarchy-replay/recording.json'
            saved_intent = state_file.read_bytes()
            discovered = call('paths')
            assert discovered['history'] == str(external) and discovered['using_last_valid_config']
            assert discovered['config_error'] and state_file.read_bytes() == saved_intent
            assert config.read_text() == '[recording]\ninterval_seconds="invalid"\n'
            assert count() == 0, 'read-only path fallback started capture'
            config.write_text(custom_config); call('reload')
            call('resume'); eventually(lambda: count() >= 2)
            call('index-pause'); retained = count()
            # Simulate a removed volume with an empty directory left at the same
            # mount path. Neither the service nor init may initialize it again.
            disconnected = external.with_name('disconnected-history')
            external.rename(disconnected); external.mkdir()
            eventually(lambda: status()['state'] == 'storage-unavailable')
            time.sleep(.7)
            assert list(external.iterdir()) == [], 'capture wrote into a replacement mount folder'
            call('init', success=False)
            assert list(external.iterdir()) == [], 'init replaced a disconnected archive'
            child.terminate(); child.wait(timeout=15); launch()
            eventually(lambda: status()['state'] == 'storage-unavailable')
            assert status()['intent'] == 'running' and status()['indexing_paused']
            assert list(external.iterdir()) == [], 'restart replaced a disconnected archive'
            external.rmdir(); disconnected.rename(external)
            eventually(lambda: count() > retained)
            assert status()['storage_available']
            # A second live switch should continue recording, while the old
            # archive stops growing and remains readable.
            second_history = root / 'second-disk-history'; second_history.mkdir()
            config.write_text(valid_config.replace('[storage]\n', '[storage]\ndirectory=' + json.dumps(str(second_history)) + '\n'))
            switched = call('reload'); previous_history = history; history = second_history
            assert switched['intent'] == 'running' and switched['indexing_paused']
            eventually(lambda: count() >= 2); call('pause')
            with sqlite3.connect(previous_history / 'index.sqlite') as db:
                previous_count = db.execute('SELECT COUNT(*) FROM observations').fetchone()[0]
            time.sleep(.7)
            with sqlite3.connect(previous_history / 'index.sqlite') as db:
                assert db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == previous_count
            with sqlite3.connect(original_history / 'index.sqlite') as db:
                assert db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == original_count
            # Seed one deliberately large fictional original, then lower the
            # allowance while paused. Opening/reloading must not evict it, but
            # resumed capture must roll it out rather than stop at the cap.
            larger_config = config.read_text().replace('max_disk_mib=64', 'max_disk_mib=128')
            config.write_text(larger_config); call('reload')
            assert status()['intent'] == 'paused' and status()['indexing_paused']
            with sqlite3.connect(history / 'index.sqlite') as db:
                old_id, old_path = db.execute('SELECT id,path FROM frames ORDER BY timestamp_ms,id LIMIT 1').fetchone()
                highest_id = db.execute('SELECT MAX(id) FROM frames').fetchone()[0]
                old_bytes = db.execute('SELECT bytes FROM history_media WHERE path=?', (old_path,)).fetchone()[0]
                padded_bytes = 72 * 1024 * 1024
                with (history / old_path).open('r+b') as media:
                    media.truncate(padded_bytes)
                db.execute('UPDATE history_media SET bytes=? WHERE path=?', (padded_bytes, old_path))
                db.execute('UPDATE frames SET source_bytes=? WHERE id=?', (padded_bytes, old_id))
                db.execute("UPDATE metadata SET value=CAST(value AS INTEGER)+? WHERE key='history_media_bytes'", (padded_bytes-old_bytes,))
            retained = count()
            config.write_text(larger_config.replace('max_disk_mib=128', 'max_disk_mib=64')); call('reload')
            time.sleep(.7)
            assert count() == retained and status()['intent'] == 'paused', 'smaller cap evicted history or resumed manual pause'
            assert (history / old_path).exists()
            resumed_at = status()['retained_this_run']
            call('resume')
            eventually(lambda: status()['retained_this_run'] >= resumed_at + 5)
            call('pause')
            assert not (history / old_path).exists(), 'oldest original remained after rolling admission'
            with sqlite3.connect(history / 'index.sqlite') as db:
                assert not db.execute('SELECT 1 FROM frames WHERE id=?', (old_id,)).fetchone()
                assert db.execute('SELECT MAX(id) FROM frames').fetchone()[0] > highest_id
                assert db.execute("SELECT COUNT(*) FROM frames WHERE ocr_state='pending'").fetchone()[0] > 0
                assert db.execute('SELECT COUNT(*) FROM observations o LEFT JOIN frames f ON f.id=o.frame_id WHERE f.id IS NULL').fetchone()[0] == 0
            actual_bytes = sum(path.stat().st_size for path in (history / 'media').iterdir())
            actual_bytes += sum(path.stat().st_size for path in history.glob('index.sqlite*'))
            assert actual_bytes <= 64 * 1024 * 1024, 'service continued beyond its allowance without reclamation'
            call('index-resume')
            eventually(lambda: status()['progress'].get('pending') == 0, 30)
            assert status()['progress'].get('failed') == 0, 'rollover broke indexing of retained/new history'
            call('stop'); call('shutdown'); child.wait(timeout=15)
            assert child.returncode == 0 and not status()['running']
            with sqlite3.connect(history / 'index.sqlite') as db:
                assert db.execute('SELECT COUNT(*) FROM history_gaps').fetchone()[0] > 0
            # A pending deletion recorded under the pre-rename default path
            # must finish against that same archive through its migration link.
            (root / 'data/oma-rewind').symlink_to(root / 'data/omarchy-replay', target_is_directory=True)
            state_file = root / 'state/omarchy-replay/recording.json'
            saved = json.loads(state_file.read_text())
            saved['deletion'] = dict(from_ms=0, to_ms=1, directory=str(root / 'data/oma-rewind/history'))
            state_file.write_text(json.dumps(saved)); config.write_text(valid_config)
            launch(); eventually(lambda: not status()['deleting'])
            assert status()['history_directory'] == str(original_history) and status()['intent'] == 'stopped'
            with sqlite3.connect(original_history / 'index.sqlite') as db:
                assert db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == original_count
            call('shutdown'); child.wait(timeout=15)
            logs.flush(); logs.seek(0)
            assert 'QIODevice::read' not in logs.read(), 'idle coordinator repeatedly read an unopened worker'
            print('PASS shared recording, lifecycle gaps, persistent controls, config recovery, deletion, indexing and safe disk switching')
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                try: child.wait(timeout=15)
                except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)
            logs.seek(0)
            if child and child.returncode not in (None, 0):
                print(logs.read()[-4000:], file=sys.stderr)
            logs.close()


if __name__ == '__main__':
    main()
