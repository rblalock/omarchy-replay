#!/usr/bin/env python3
"""Prove copied runtimes work without source and reject inconsistent payloads."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else ROOT / 'build/replay'
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts'))
from package_runtime import MANIFEST_NAME, PAYLOAD, content_hash, file_hash, stage_runtime, verify_runtime
from runtime_layout import VERSION, native_binary


class RuntimePackageTest(unittest.TestCase):
    def test_plugin_package_and_native_versions_agree(self):
        self.assertEqual(json.loads((ROOT / 'manifest.json').read_text())['version'], VERSION)
        result = subprocess.run([str(BINARY), '--version'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), f'Omarchy Replay {VERSION}')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='replay-runtime-package-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'checkout'
        self.source.mkdir()
        (self.source / 'CMakeLists.txt').write_text('project(omarchy_replay VERSION 0.1.0)\n')
        for relative in PAYLOAD:
            source = BINARY if relative == 'bin/replay' else ROOT / relative
            target = self.source / ('build/replay' if relative == 'bin/replay' else relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        # None of these checkout-only files may appear in the package.
        for relative in ('runs/private.txt', 'src/private.cpp', '.env', 'build/private-cache', 'tests/private.py'):
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('NEVER PACKAGE THIS')
        self.destination = self.root / 'standalone runtime'
        self.environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', QT_QPA_PLATFORM='offscreen', OMP_THREAD_LIMIT='1')
        for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
            path = self.root / name
            path.mkdir(mode=0o700)
            self.environment[name] = str(path)

    def command(self, *args):
        return subprocess.run(args, capture_output=True, text=True, env=self.environment, timeout=8, cwd=self.root)

    def test_runtime_survives_checkout_removal_and_has_no_build_fallback(self):
        manifest = stage_runtime(self.source, self.destination)
        self.assertEqual(manifest['version'], VERSION)
        self.assertEqual(set(manifest['files']), set(PAYLOAD))
        self.assertIsNone(manifest['source_revision'])
        shutil.rmtree(self.source)
        wrapper = self.destination / 'scripts/replay'
        version = self.command(str(wrapper), '--version')
        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertEqual(version.stdout.strip(), f'Omarchy Replay {VERSION}')
        imports = self.command(sys.executable, '-B', '-c',
            'import sys; sys.path.insert(0, sys.argv[1]); import open_replay, viewer_launch, plugin_control; '
            'assert "trial" not in sys.modules and "measure" not in sys.modules', str(self.destination / 'scripts'))
        self.assertEqual(imports.returncode, 0, imports.stderr)
        entry = self.command(str(wrapper), 'desktop-entry')
        self.assertEqual(entry.returncode, 0, entry.stderr)
        self.assertIn(str(wrapper), entry.stdout)
        paths = self.command(str(wrapper), 'daemon', 'paths')
        self.assertEqual(paths.returncode, 0, paths.stderr)
        self.assertIn(self.environment['XDG_CONFIG_HOME'], paths.stdout)
        self.assertEqual(verify_runtime(self.destination), manifest)
        build = self.command(str(wrapper), 'build')
        self.assertNotEqual(build.returncode, 0)
        self.assertIn('cannot build source', build.stderr)
        self.assertFalse((self.destination / 'build').exists())
        (self.destination / 'bin/replay').unlink()
        missing = self.command(str(wrapper), '--version')
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('executable is missing', missing.stderr)
        self.assertFalse((self.destination / 'build').exists())
        with self.assertRaisesRegex(RuntimeError, 'executable is missing'):
            native_binary(self.destination)

    def test_verified_runtime_can_be_restaged_without_source(self):
        manifest = stage_runtime(self.source, self.destination)
        shutil.rmtree(self.source)
        copied = self.root / 'copy'
        self.assertEqual(stage_runtime(self.destination, copied), manifest)
        self.assertEqual(verify_runtime(copied), manifest)
        with self.assertRaisesRegex(RuntimeError, 'already exists'):
            stage_runtime(self.destination, copied)

    def test_cached_source_build_does_not_require_a_checkout_build_directory(self):
        shutil.rmtree(self.source / 'build')
        stage_runtime(self.source, self.destination, binary=BINARY)
        self.assertEqual(verify_runtime(self.destination)['version'], VERSION)
        with self.assertRaisesRegex(RuntimeError, 'cannot replace its binary'):
            stage_runtime(self.destination, self.root / 'replacement', binary=BINARY)

    def test_stale_build_version_is_rejected_before_creating_destination(self):
        binary = self.source / 'build/replay'
        binary.write_text('#!/bin/sh\nprintf "Omarchy Replay 0.0.0\\n"\n')
        binary.chmod(0o755)
        with self.assertRaisesRegex(RuntimeError, 'version differs'):
            stage_runtime(self.source, self.destination)
        self.assertFalse(self.destination.exists())

    def test_changed_data_modes_and_extra_entries_are_rejected(self):
        for damage in ('text', 'mode', 'extra', 'symlink', 'traversal', 'digest'):
            with self.subTest(damage=damage):
                destination = self.root / damage
                stage_runtime(self.source, destination)
                if damage == 'text':
                    with (destination / 'LICENSE').open('a') as stream:
                        stream.write('corrupt')
                elif damage == 'mode':
                    (destination / 'bin/replay').chmod(0o644)
                elif damage == 'extra':
                    (destination / 'extra.py').write_text('extra')
                elif damage == 'symlink':
                    (destination / 'LICENSE').unlink()
                    (destination / 'LICENSE').symlink_to(self.source / 'LICENSE')
                elif damage == 'traversal':
                    path = destination / MANIFEST_NAME
                    manifest = json.loads(path.read_text())
                    manifest['files']['../outside'] = manifest['files'].pop('LICENSE')
                    path.write_text(json.dumps(manifest))
                else:
                    path = destination / MANIFEST_NAME
                    manifest = json.loads(path.read_text())
                    manifest['content_hash'] = '0' * 64
                    path.write_text(json.dumps(manifest))
                with self.assertRaises(RuntimeError):
                    verify_runtime(destination)

    def test_existing_runtime_with_older_payload_set_verifies_and_unknown_paths_are_rejected(self):
        destination = self.root / 'older'
        stage_runtime(self.source, destination)
        path = destination / MANIFEST_NAME
        manifest = json.loads(path.read_text())
        # A 0.1.0-style installation: no scripts/replay_mcp.py anywhere.
        manifest['files'].pop('scripts/replay_mcp.py')
        (destination / 'scripts/replay_mcp.py').unlink()
        manifest['content_hash'] = content_hash(manifest['version'], manifest['files'])
        path.write_text(json.dumps(manifest))
        self.assertEqual(verify_runtime(destination, existing=True)['files'], manifest['files'])
        with self.assertRaisesRegex(RuntimeError, 'approved payload'):
            verify_runtime(destination)
        # An unknown path in the manifest is still rejected, even when hashed correctly.
        older = json.loads(path.read_text())
        (destination / 'scripts/unknown.py').write_bytes(b'x' * 4096)
        unknown = destination / 'scripts/unknown.py'
        unknown.chmod(0o644)
        older['files']['scripts/unknown.py'] = {'sha256': file_hash(unknown), 'mode': 0o644}
        older['content_hash'] = content_hash(older['version'], older['files'])
        path.write_text(json.dumps(older))
        with self.assertRaisesRegex(RuntimeError, 'approved payload'):
            verify_runtime(destination, existing=True)
        # An extra file on disk without a manifest entry is still rejected.
        path.write_text(json.dumps(manifest))
        (destination / 'extra.py').write_text('extra')
        with self.assertRaisesRegex(RuntimeError, 'Unexpected runtime entry'):
            verify_runtime(destination, existing=True)

    def test_symlinked_source_is_rejected_before_creating_destination(self):
        path = self.source / 'LICENSE'
        path.unlink()
        path.symlink_to(ROOT / 'LICENSE')
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            stage_runtime(self.source, self.destination)
        self.assertFalse(self.destination.exists())

    def test_failed_stage_removes_partial_payload_and_preserves_source(self):
        copy = shutil.copyfile
        def interrupted(source, destination):
            if Path(destination).name == 'open_replay.py':
                raise OSError('simulated full disk')
            return copy(source, destination)
        with patch('package_runtime.shutil.copyfile', side_effect=interrupted):
            with self.assertRaisesRegex(OSError, 'full disk'):
                stage_runtime(self.source, self.destination)
        self.assertFalse(self.destination.exists())
        self.assertTrue((self.source / 'build/replay').is_file())


if __name__ == '__main__':
    unittest.main()
