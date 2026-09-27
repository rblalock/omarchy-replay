#!/usr/bin/env python3
"""Stage or verify the standalone Replay runtime. Never installs or records."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys

from runtime_layout import ROOT, VERSION, native_binary

MANIFEST_NAME = 'runtime-manifest.json'
# Only runtime entrypoints and their local imports belong in an installation.
# A repository file added elsewhere can never become an accidental release asset.
PAYLOAD = {
    'bin/replay': 0o755,
    'scripts/replay': 0o755,
    'scripts/open_replay.py': 0o644,
    'scripts/replay_mcp.py': 0o644,
    'scripts/viewer_launch.py': 0o644,
    'scripts/runtime_layout.py': 0o644,
    'scripts/package_runtime.py': 0o644,
    'scripts/plugin_control.py': 0o644,
    'scripts/install_recording_service.py': 0o644,
    'scripts/install_capture_exclusions.py': 0o644,
    'scripts/install_viewer_exclusion.py': 0o644,
    'scripts/migrate_replay_paths.py': 0o644,
    'config/hypr/replay-viewer.lua': 0o644,
    'config/hypr/replay-window.lua': 0o644,
    'LICENSE': 0o644,
    'THIRD_PARTY_NOTICES.md': 0o644,
    'docs/agent-guide.md': 0o644,
}


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def content_hash(version, files):
    canonical = json.dumps({'version': version, 'files': files}, sort_keys=True,
                           separators=(',', ':'), ensure_ascii=True).encode()
    return hashlib.sha256(canonical).hexdigest()


def checked_file(root, relative):
    """Do not follow payload symlinks, including parent-directory symlinks."""
    current = root
    for part in Path(relative).parts:
        current = current / part
        if current.is_symlink():
            raise RuntimeError(f'Runtime payload contains a symlink: {relative}')
    if not current.is_file():
        raise RuntimeError(f'Runtime payload is missing: {relative}')
    return current


def verify_runtime(root, existing=False):
    """Return a verified manifest, or raise RuntimeError for an invalid runtime.

    Hashes detect damage or inconsistent copies; they are not publisher signatures.
    No payload code is executed during verification.

    An already-installed runtime may predate the current PAYLOAD (an upgrade from
    an older release): existing=True accepts a manifest whose file set is a
    subset of PAYLOAD, still rejecting unknown paths and checking every listed
    file's hash and mode. Staging a new payload always requires an exact match.
    """
    root = Path(root).resolve()
    try:
        path = checked_file(root, MANIFEST_NAME)
        if path.stat().st_size > 128 * 1024:
            raise RuntimeError('Runtime manifest is too large.')
        manifest = json.loads(path.read_bytes())
        if not isinstance(manifest, dict) or set(manifest) != {
                'schema_version', 'version', 'source_revision', 'source_dirty', 'files', 'content_hash'}:
            raise RuntimeError('Invalid runtime manifest fields.')
        if type(manifest['schema_version']) is not int or manifest['schema_version'] != 1:
            raise RuntimeError('Unsupported runtime manifest schema.')
        version = manifest['version']
        if not isinstance(version, str) or not re.fullmatch(r'\d+\.\d+\.\d+', version):
            raise RuntimeError('Invalid runtime version.')
        revision = manifest['source_revision']
        if revision is not None and (not isinstance(revision, str) or not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', revision)):
            raise RuntimeError('Invalid source revision.')
        if manifest['source_dirty'] is not None and type(manifest['source_dirty']) is not bool:
            raise RuntimeError('Invalid source state.')
        files = manifest['files']
        if not isinstance(files, dict) or not files:
            raise RuntimeError('Runtime manifest does not match the approved payload.')
        if existing:
            if not set(files) <= set(PAYLOAD):
                raise RuntimeError('Runtime manifest does not match the approved payload.')
        elif set(files) != set(PAYLOAD):
            raise RuntimeError('Runtime manifest does not match the approved payload.')
        for relative, details in files.items():
            expected_mode = PAYLOAD[relative]
            if not isinstance(details, dict) or set(details) != {'sha256', 'mode'}:
                raise RuntimeError(f'Invalid manifest entry: {relative}')
            if type(details['mode']) is not int or details['mode'] != expected_mode:
                raise RuntimeError(f'Invalid payload mode: {relative}')
            digest = details['sha256']
            if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
                raise RuntimeError(f'Invalid payload hash: {relative}')
            payload = checked_file(root, relative)
            if stat.S_IMODE(payload.stat().st_mode) != expected_mode or file_hash(payload) != digest:
                raise RuntimeError(f'Runtime payload differs from its manifest: {relative}')
        expected_files = set(files) | {MANIFEST_NAME}
        expected_dirs = {str(parent) for item in expected_files for parent in Path(item).parents if str(parent) != '.'}
        for path in root.rglob('*'):
            relative = str(path.relative_to(root))
            if path.is_symlink() or (relative not in expected_files and relative not in expected_dirs):
                raise RuntimeError(f'Unexpected runtime entry: {relative}')
        if manifest['content_hash'] != content_hash(version, files):
            raise RuntimeError('Runtime content hash does not match its manifest.')
        return manifest
    except (OSError, ValueError, TypeError) as error:
        raise RuntimeError(f'Cannot verify Replay runtime: {error}') from error


def source_identity(root):
    """Informational provenance, never a claim that Git authenticates a payload."""
    try:
        top = subprocess.run(['git', '-C', str(root), 'rev-parse', '--show-toplevel'],
                             capture_output=True, text=True, timeout=5)
        if top.returncode or Path(top.stdout.strip()).resolve() != root:
            return None, None
        revision = subprocess.run(['git', '-C', str(root), 'rev-parse', '--verify', 'HEAD'],
                                  check=True, capture_output=True, text=True, timeout=5).stdout.strip()
        if not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', revision):
            return None, None
        changed = subprocess.run(['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=normal',
                                  '--', 'CMakeLists.txt', 'src', 'scripts', 'config', 'docs/agent-guide.md',
                                  'LICENSE', 'THIRD_PARTY_NOTICES.md'],
                                 check=True, capture_output=True, timeout=5).stdout
        return revision, bool(changed)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None, None


def stage_runtime(source_root, destination, binary=None):
    """Copy a source build or verified runtime into a new standalone directory.

    The destination must not exist. A failed stage removes only the directory
    created by this call. An optional binary selects an existing source build
    outside the checkout. Callers choose when to activate the verified result.
    """
    source_root = Path(source_root).resolve()
    destination = Path(destination).absolute()
    if destination.exists() or destination.is_symlink():
        raise RuntimeError('Runtime staging destination already exists.')
    packaged = (source_root / MANIFEST_NAME).exists()
    original = verify_runtime(source_root) if packaged else None
    if not packaged and not (source_root / 'CMakeLists.txt').is_file():
        raise RuntimeError('Source runtime is not a Replay build or verified package.')
    version = original['version'] if original else VERSION
    if original and binary is not None:
        raise RuntimeError('A verified runtime cannot replace its binary while being restaged.')
    if binary is None:
        binary = native_binary(source_root)
    else:
        binary = Path(binary).expanduser().absolute()
        if binary.is_symlink() or not binary.is_file() or not os.access(binary, os.X_OK):
            raise RuntimeError('The supplied Replay build is not a regular executable.')
        binary = binary.resolve()
    if not original:
        try:
            reported = subprocess.run([str(binary), '--version'], check=True, capture_output=True,
                                      text=True, timeout=5, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        except (OSError, subprocess.SubprocessError) as error:
            raise RuntimeError('Cannot check the Replay build version. Rebuild before installing.') from error
        if reported.stdout.strip() != f'Omarchy Replay {version}':
            raise RuntimeError('Replay build version differs from its source. Rebuild before installing.')
    sources = {}
    for relative in PAYLOAD:
        sources[relative] = checked_file(binary.parent, binary.name) if relative == 'bin/replay' else checked_file(source_root, relative)
    destination.mkdir(parents=True, exist_ok=False)
    try:
        files = {}
        for relative, mode in PAYLOAD.items():
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(sources[relative], target)
            target.chmod(mode)
            files[relative] = {'sha256': file_hash(target), 'mode': mode}
        revision, dirty = (original['source_revision'], original['source_dirty']) if original else source_identity(source_root)
        manifest = {'schema_version': 1, 'version': version, 'source_revision': revision, 'source_dirty': dirty, 'files': files,
                    'content_hash': content_hash(version, files)}
        if original and manifest != original:
            raise RuntimeError('Source runtime changed while it was being staged.')
        manifest_path = destination / MANIFEST_NAME
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        manifest_path.chmod(0o644)
        return verify_runtime(destination)
    except BaseException:
        shutil.rmtree(destination)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    command = parser.add_subparsers(dest='command', required=True)
    stage = command.add_parser('stage', help='copy a standalone runtime into a new directory')
    stage.add_argument('destination', type=Path)
    stage.add_argument('--source-root', type=Path, default=ROOT)
    stage.add_argument('--binary', type=Path, help='use an existing native source build outside the checkout')
    verify = command.add_parser('verify', help='check an existing runtime without executing it')
    verify.add_argument('root', type=Path)
    args = parser.parse_args(argv)
    try:
        result = stage_runtime(args.source_root, args.destination, args.binary) if args.command == 'stage' else verify_runtime(args.root)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (OSError, RuntimeError) as error:
        print(f'Replay package: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
