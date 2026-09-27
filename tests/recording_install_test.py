#!/usr/bin/env python3
"""Native installer transactions without changing the actual desktop."""
import json
import io
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import install_recording_service as installer


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='replay-service-install-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / 'hypr/bindings.lua'
        self.path.parent.mkdir()
        self.original = '-- User settings\no.bind("SUPER + A", "App", "some-app")\n'
        self.path.write_text(self.original)
        self.path.chmod(0o640)

    def test_shortcut_is_idempotent_and_preserves_existing_settings(self):
        with patch.object(installer, 'hyprctl', return_value='[]'), patch.object(installer, 'validate'):
            installer.install_shortcut(self.root)
            once = self.path.read_text()
            installer.install_shortcut(self.root)
        self.assertEqual(once, self.path.read_text())
        self.assertTrue(once.startswith(self.original))
        self.assertEqual(once.count(installer.BEGIN), 1)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o640)
        self.assertIn('open --toggle --notify-errors', once)

    def test_existing_binding_symlink_and_concurrent_edits_are_preserved(self):
        with patch.object(installer, 'hyprctl', return_value=json.dumps([{'modmask': 72, 'key': 'R'}])):
            with self.assertRaisesRegex(RuntimeError, 'already bound'):
                installer.install_shortcut(self.root)
        self.assertEqual(self.path.read_text(), self.original)
        with patch.object(installer, 'hyprctl', return_value='[]'), \
             patch.object(installer, 'validate', side_effect=lambda: self.path.write_text('new user edit\n')):
            with self.assertRaisesRegex(RuntimeError, 'Bindings changed'):
                installer.install_shortcut(self.root)
        self.assertEqual(self.path.read_text(), 'new user edit\n')
        self.path.unlink(); target = self.root / 'linked.lua'; target.write_text(self.original); self.path.symlink_to(target)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            installer.install_shortcut(self.root)
        self.assertEqual(target.read_text(), self.original)

    def test_validation_failure_rolls_back_owned_change(self):
        with patch.object(installer, 'hyprctl', return_value='[]'), \
             patch.object(installer, 'validate', side_effect=[None, RuntimeError('bad config')]):
            with self.assertRaisesRegex(RuntimeError, 'bad config'):
                installer.install_shortcut(self.root)
        self.assertEqual(self.path.read_text(), self.original)

    def test_invalid_xdg_values_match_native_fallback(self):
        fallback = self.root / 'fallback'
        for value in ('', 'relative/path', '~/config'):
            with patch.dict(os.environ, {'XDG_CONFIG_HOME': value}):
                self.assertEqual(installer.xdg_path('XDG_CONFIG_HOME', fallback), fallback)

    def test_managed_unit_quoting_and_unmanaged_file_are_safe(self):
        text = installer.unit_text(Path('/tmp/space dir/100%/replay'))
        self.assertIn('ExecStart="/tmp/space dir/100%%/replay" daemon run', text)
        self.assertIn('KillMode=control-group', text)
        target = self.root / 'existing.service'; target.write_text('user service\n')
        with self.assertRaisesRegex(RuntimeError, 'not managed'):
            installer.managed_write(target, 'replacement', '# Managed', 0o600)
        self.assertEqual(target.read_text(), 'user service\n')


class TransactionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='replay-installer-transaction-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'home space % ü'
        self.root.mkdir()
        env = {'HOME': str(self.root), **{key: str(self.root / key) for key in
            ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR')}}
        self.environment = patch.dict(os.environ, env)
        self.environment.start(); self.addCleanup(self.environment.stop)
        self.p = installer.paths()
        self.p['hypr'].parent.mkdir(parents=True)
        self.original_hypr = b'-- User desktop\n'
        self.original_bindings = b'-- User bindings\no.bind("SUPER + A", "Other", "other")\n'
        self.p['hypr'].write_bytes(self.original_hypr)
        self.p['bindings'].write_bytes(self.original_bindings)
        self.active = False
        self.enabled = False
        self.commands = []
        self.fail_action = None
        self.fail_reload = False
        self.hash = 'a' * 64
        self.output = io.StringIO(); self.errors = io.StringIO()
        import package_runtime
        self.package = package_runtime
        for context in (patch.object(installer, 'preflight_runtime', side_effect=lambda binary: installer.validate()),
                        patch.object(installer, 'validate'),
                        patch.object(installer, 'hyprctl', side_effect=self.hypr),
                        patch.object(installer.subprocess, 'run', side_effect=self.run_process),
                        patch.object(package_runtime, 'stage_runtime', side_effect=self.stage),
                        patch.object(package_runtime, 'verify_runtime', side_effect=self.verify),
                        patch.object(sys, 'stdout', self.output), patch.object(sys, 'stderr', self.errors)):
            context.start(); self.addCleanup(context.stop)

    def hypr(self, *args):
        if args == ('reload',) and self.fail_reload:
            self.fail_reload = False
            raise RuntimeError('synthetic reload failure')
        return '[]'

    def stage(self, source, destination, binary=None):
        destination.mkdir(parents=True)
        (destination / 'bin').mkdir()
        (destination / 'bin/replay').write_text('synthetic executable')
        (destination / 'bin/replay').chmod(0o755)
        rules = destination / 'config/hypr'; rules.mkdir(parents=True)
        for name in ('replay-window.lua', 'replay-viewer.lua'):
            (rules / name).write_text('-- Managed by Omarchy Replay: synthetic\n')
        if (source / 'runtime-manifest.json').exists():
            manifest = self.verify(source)
        else:
            manifest = {'schema_version': 1, 'version': '0.1.0', 'content_hash': self.hash, 'files': {}}
        (destination / 'runtime-manifest.json').write_text(json.dumps(manifest))
        return manifest

    def verify(self, root, existing=False):
        return json.loads((root / 'runtime-manifest.json').read_text())

    def run_process(self, command, **kwargs):
        self.commands.append(command)
        code = 0; output = ''
        if command[:2] == ['systemctl', '--user']:
            action = command[2]
            if action == self.fail_action:
                self.fail_action = None
                raise subprocess.CalledProcessError(1, command)
            if action == 'is-active':
                code = 0 if self.active else 3
            elif action == 'is-enabled':
                code = 0 if self.enabled else 1
                output = 'enabled\n' if self.enabled else 'disabled\n'
            elif action == 'show' and '--property=MainPID' in command:
                output = '4242\n' if self.active else '0\n'
            elif action == 'stop':
                self.active = False
            elif action == 'start':
                self.active = True
            elif action == 'enable':
                self.enabled = True
            elif action == 'disable':
                self.enabled = False
        elif 'daemon' in command:
            action = command[2]
            if action == 'init':
                self.p['settings'].parent.mkdir(parents=True, exist_ok=True)
                self.p['settings'].write_text('[service]\nlogin_startup = false\n')
                self.p['settings'].chmod(0o600)
            elif action == 'paths':
                output = json.dumps({'config_error': '', 'using_last_valid_config': False})
            elif action == 'status':
                saved = json.loads(self.p['recording'].read_text()) if self.p['recording'].exists() else {}
                output = json.dumps({'running': self.active, 'pid': 4242, **saved})
        return subprocess.CompletedProcess(command, code, output if kwargs.get('text') else output.encode(), '')

    def invoke(self, *args, success=True):
        result = installer.main(list(args))
        self.assertEqual(result, 0 if success else 1, self.errors.getvalue())
        return result

    def existing(self, intent='running', active=True, enabled=True):
        self.p['settings'].parent.mkdir(parents=True, exist_ok=True)
        self.p['settings'].write_text('[service]\nlogin_startup = ' + str(enabled).lower() + '\n[unknown]\nkeep = 42\n')
        self.p['settings'].chmod(0o600)
        self.p['unit'].parent.mkdir(parents=True, exist_ok=True)
        self.p['unit'].write_text(installer.UNIT_MARKER + 'old managed unit\n')
        self.p['unit'].chmod(0o600)
        self.p['recording'].parent.mkdir(parents=True, exist_ok=True)
        self.p['recording'].write_text(json.dumps({'intent': intent, 'indexing_paused': True, 'unknown': 7}))
        self.p['recording'].chmod(0o600)
        self.active, self.enabled = active, enabled
        history = self.p['data'] / 'omarchy-replay/history'
        history.mkdir(parents=True, exist_ok=True)
        (history / 'index.sqlite').write_bytes(b'synthetic history must survive')

    def test_fresh_install_is_stopped_and_uses_installed_payload(self):
        self.invoke('--output', 'DP-test')
        self.assertFalse(self.active)
        self.assertFalse(self.enabled)
        self.assertIn('app/current/bin/replay', self.p['unit'].read_text())
        self.assertNotIn('/build/', self.p['unit'].read_text())
        self.assertIn('app/current/scripts/replay', self.p['launcher'].read_text())
        self.assertIn(' open --toggle --notify-errors', self.p['bindings'].read_text())
        self.assertTrue((self.p['app'] / 'current').is_symlink())
        self.assertTrue(self.p['hypr'].read_bytes().startswith(self.original_hypr))
        self.assertEqual(self.p['launcher'].stat().st_mode & 0o777, 0o755)

    def test_update_preserves_capture_indexing_settings_and_previous_payload(self):
        for intent in ('running', 'paused', 'stopped'):
            with self.subTest(intent=intent):
                self.existing(intent)
                original = self.p['recording'].read_bytes()
                config = self.p['settings'].read_bytes()
                self.invoke()
                first = (self.p['app'] / 'current').resolve()
                self.hash = 'b' * 64
                self.invoke('--no-shortcut')
                self.assertTrue(first.is_dir())
                self.assertTrue(self.active)
                self.assertTrue(self.enabled)
                self.assertEqual(original, self.p['recording'].read_bytes())
                self.assertEqual(config, self.p['settings'].read_bytes())
                self.assertEqual((self.p['data'] / 'omarchy-replay/history/index.sqlite').read_bytes(), b'synthetic history must survive')

    def test_update_preserves_custom_stable_launcher_shortcut(self):
        self.invoke()
        custom = self.p['bindings'].read_text().replace('SUPER + ALT + R', 'SUPER + CTRL + Y')
        self.p['bindings'].write_text(custom)
        self.hash = 'd' * 64
        self.invoke()
        self.assertEqual(self.p['bindings'].read_text(), custom)

    def test_old_checkout_shortcut_migrates_and_ambiguous_markers_fail(self):
        checkout = installer.shortcut_text(self.original_bindings.decode(), Path('/old checkout/scripts/replay'))
        self.p['bindings'].write_text(checkout)
        self.invoke()
        current = self.p['bindings'].read_text()
        self.assertNotIn('/old checkout/', current)
        self.assertIn(installer.lua_string(__import__('shlex').quote(str(self.p['launcher'])) + ' open --toggle --notify-errors'), current)
        self.p['bindings'].write_text(current + installer.BEGIN + '\n')
        self.invoke(success=False)
        self.assertEqual(self.p['bindings'].read_text(), current + installer.BEGIN + '\n')

    def test_offline_update_does_not_start_service(self):
        self.existing(active=False, enabled=False)
        self.invoke()
        self.assertFalse(self.active)
        self.assertNotIn(['systemctl', '--user', 'start', 'omarchy-replay.service'], self.commands)

    def test_restart_failure_restores_old_unit_files_enablement_and_activity(self):
        self.existing()
        self.invoke()
        old_target = os.readlink(self.p['app'] / 'current')
        old_files = {name: self.p[name].read_bytes() for name in ('unit', 'launcher', 'desktop', 'hypr', 'bindings', 'recording', 'settings')}
        self.hash = 'c' * 64
        self.fail_action = 'start'
        self.invoke(success=False)
        self.assertTrue(self.active)
        self.assertTrue(self.enabled)
        self.assertEqual(os.readlink(self.p['app'] / 'current'), old_target)
        for name, data in old_files.items():
            self.assertEqual(self.p[name].read_bytes(), data, name)

    def test_reload_failure_restores_source_install(self):
        self.existing()
        unit = self.p['unit'].read_bytes()
        self.fail_reload = True
        self.invoke(success=False)
        self.assertTrue(self.active)
        self.assertTrue(self.enabled)
        self.assertEqual(unit, self.p['unit'].read_bytes())
        self.assertEqual(self.original_hypr, self.p['hypr'].read_bytes())
        self.assertEqual(self.original_bindings, self.p['bindings'].read_bytes())
        self.assertFalse(self.p['desktop'].exists())
        self.assertFalse(self.p['launcher'].exists())
        self.assertFalse((self.p['app'] / 'current').exists())

    def test_unmanaged_path_and_collision_fail_before_stopping(self):
        self.existing()
        self.p['launcher'].parent.mkdir(parents=True)
        self.p['launcher'].write_text('unrelated user program\n')
        self.invoke(success=False)
        self.assertTrue(self.active)
        self.assertNotIn(['systemctl', '--user', 'stop', 'omarchy-replay.service'], self.commands)
        self.assertEqual(self.p['launcher'].read_text(), 'unrelated user program\n')
        self.p['launcher'].unlink()
        with patch.object(installer, 'hyprctl', return_value='[{"modmask":72,"key":"R"}]'):
            self.invoke(success=False)
        self.assertTrue(self.active)

    def test_manual_enablement_override_is_preserved_on_update(self):
        self.existing(enabled=True)
        self.enabled = False
        self.invoke()
        self.assertFalse(self.enabled)
        self.assertIn('login_startup = true', self.p['settings'].read_text())

    def test_ready_service_requires_matching_systemd_identity(self):
        prior = {'intent': 'paused', 'indexing_paused': True}
        with patch.object(installer, 'systemctl', return_value=subprocess.CompletedProcess([], 0, '9999', '')), \
             patch.object(installer.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps({'running': True, 'pid': 8888, **prior}), '')), \
             patch.object(installer.time, 'monotonic', side_effect=[0, 0, 11]), patch.object(installer.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'expected systemd identity'):
                installer.wait_for_service(Path('/fake/replay'), prior)

    def test_uninstall_preserves_history_settings_and_stops_future_capture(self):
        self.existing(); self.invoke()
        config = self.p['settings'].read_bytes()
        self.invoke('--uninstall')
        self.assertFalse(self.active); self.assertFalse(self.enabled)
        self.assertEqual(self.p['settings'].read_bytes(), config)
        saved = json.loads(self.p['recording'].read_text())
        self.assertEqual(saved, {'intent': 'stopped', 'indexing_paused': True, 'unknown': 7})
        self.assertEqual(self.original_hypr, self.p['hypr'].read_bytes())
        self.assertEqual(self.original_bindings, self.p['bindings'].read_bytes())
        for name in ('unit', 'desktop', 'launcher', 'app'):
            self.assertFalse(self.p[name].exists(), name)
        self.assertEqual((self.p['data'] / 'omarchy-replay/history/index.sqlite').read_bytes(), b'synthetic history must survive')
        self.invoke('--uninstall')
        self.invoke()
        self.assertFalse(self.active)
        self.assertEqual(json.loads(self.p['recording'].read_text())['intent'], 'stopped')

    def test_uninstall_failure_rolls_back_intent_and_integration(self):
        self.existing(); self.invoke()
        intent = self.p['recording'].read_bytes(); unit = self.p['unit'].read_bytes()
        self.fail_reload = True
        self.invoke('--uninstall', success=False)
        self.assertTrue(self.active); self.assertTrue(self.enabled)
        self.assertEqual(self.p['recording'].read_bytes(), intent)
        self.assertEqual(self.p['unit'].read_bytes(), unit)
        self.assertTrue((self.p['app'] / 'current').exists())

    def test_rollback_preserves_concurrent_edits(self):
        target = self.root / 'concurrent'; target.write_bytes(b'original')
        tx = installer.FileTransaction(); tx.plan(target, b'ours'); tx.apply()
        target.write_bytes(b'new user content')
        with self.assertRaisesRegex(RuntimeError, 'concurrent edits'):
            tx.rollback()
        self.assertEqual(target.read_bytes(), b'new user content')

    def test_rollback_retains_rule_referenced_by_concurrent_desktop_edit(self):
        rule = self.root / 'owned.lua'
        desktop = self.root / 'desktop.lua'; desktop.write_bytes(b'original\n')
        tx = installer.FileTransaction()
        tx.plan(rule, b'our valid rule\n')
        tx.plan(desktop, ('dofile(' + installer.lua_string(str(rule)) + ')\n').encode())
        tx.references[rule] = desktop
        tx.apply()
        desktop.write_bytes(desktop.read_bytes() + b'-- concurrent user edit\n')
        with self.assertRaisesRegex(RuntimeError, 'concurrent edits'):
            tx.rollback()
        self.assertTrue(rule.exists())
        self.assertIn(b'concurrent user edit', desktop.read_bytes())

    def test_current_pointer_recovers_even_when_other_rollback_conflicts(self):
        self.existing(); self.invoke()
        app = self.p['app']; prior = os.readlink(app / 'current')
        installer.replace_pointer(app, 'versions/new', prior)
        target = self.root / 'concurrent'; target.write_bytes(b'old')
        tx = installer.FileTransaction(); tx.plan(target, b'ours'); tx.apply(); target.write_bytes(b'user')
        lease = installer.CoordinatorLease(self.p)
        failed = installer.recover_install(tx, self.p, {'exists': True, 'active': True, 'enabled': True}, lease, True, prior, 'versions/new')
        self.assertTrue(failed)
        self.assertEqual(os.readlink(app / 'current'), prior)
        self.assertEqual(target.read_bytes(), b'user')

    def test_external_payload_pointer_and_symlink_are_rejected(self):
        self.existing(); self.invoke()
        pointer = self.p['app'] / 'current'; pointer.unlink(); pointer.symlink_to(self.root)
        self.invoke(success=False)
        self.assertTrue(self.active)
        self.assertEqual(pointer.resolve(), self.root)

    def test_unexpected_app_entry_rejected_before_uninstall_mutation(self):
        self.existing(); self.invoke(); self.commands.clear()
        extra = self.p['app'] / 'notes.txt'; extra.write_bytes(b'user file')
        unit = self.p['unit'].read_bytes()
        self.invoke('--uninstall', success=False)
        self.assertEqual(extra.read_bytes(), b'user file')
        self.assertEqual((self.p['app'] / '.managed').read_bytes(), installer.APP_MARKER)
        self.assertEqual(self.p['unit'].read_bytes(), unit)
        self.assertTrue(self.active)
        self.assertNotIn(['systemctl', '--user', 'stop', 'omarchy-replay.service'], self.commands)
        self.assertIn('Unexpected files', self.errors.getvalue())

    def test_redirected_versions_rejected_without_touching_external_payload(self):
        self.existing(); self.invoke(); self.commands.clear()
        versions = self.p['app'] / 'versions'
        external = self.root / 'external-runtime'; versions.rename(external)
        versions.symlink_to(external, target_is_directory=True)
        before = {str(path.relative_to(external)): path.read_bytes() for path in external.rglob('*') if path.is_file()}
        self.invoke('--uninstall', success=False)
        self.assertTrue(self.active)
        self.assertTrue(versions.is_symlink())
        self.assertEqual(before, {str(path.relative_to(external)): path.read_bytes() for path in external.rglob('*') if path.is_file()})
        self.assertTrue(self.p['unit'].is_file())
        self.assertTrue((self.p['app'] / '.managed').is_file())
        self.assertNotIn(['systemctl', '--user', 'stop', 'omarchy-replay.service'], self.commands)

    def test_unowned_versions_directory_rejected_before_service_stop(self):
        from types import SimpleNamespace
        import stat
        self.existing(); self.invoke(); self.commands.clear()
        versions = self.p['app'] / 'versions'
        original = Path.lstat
        def metadata(path):
            if path == versions:
                return SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_uid=os.getuid() + 1)
            return original(path)
        with patch.object(Path, 'lstat', metadata):
            self.invoke('--uninstall', success=False)
        self.assertTrue(self.active)
        self.assertTrue((self.p['app'] / '.managed').is_file())
        self.assertNotIn(['systemctl', '--user', 'stop', 'omarchy-replay.service'], self.commands)

    def test_open_owned_viewer_blocks_uninstall_before_service_stop(self):
        self.existing(); self.invoke(); self.commands.clear()
        with patch.object(installer, 'has_owned_viewer', return_value=True):
            self.invoke('--uninstall', success=False)
        self.assertTrue(self.active)
        self.assertNotIn(['systemctl', '--user', 'stop', 'omarchy-replay.service'], self.commands)

    def test_locked_coordinator_blocks_integration_changes(self):
        self.existing(active=False)
        self.p['runtime'].mkdir(parents=True, mode=0o700)
        import fcntl
        lock = self.p['runtime'] / 'coordinator.lock'; lock.touch(mode=0o600)
        unit = self.p['unit'].read_bytes()
        with lock.open('rb') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.invoke(success=False)
        self.assertEqual(self.p['unit'].read_bytes(), unit)
        self.assertFalse(self.active)


if __name__ == '__main__':
    unittest.main()
