#!/usr/bin/env python3
"""Install or remove Replay's desktop integration while preserving screen history."""
import sys
sys.dont_write_bytecode = True
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import tempfile
import time
import tomllib

from install_viewer_exclusion import atomic_write, backup, hyprctl, lua_string, validate, updated_config
from install_capture_exclusions import read_policy, render_rules
from open_replay import desktop_entry, xdg_path

ROOT = Path(__file__).resolve().parents[1]
BEGIN = '-- BEGIN Omarchy Replay summon'
END = '-- END Omarchy Replay summon'
UNIT_MARKER = '# Managed by Omarchy Replay\n'
LAUNCHER_MARKER = '# Managed by Omarchy Replay launcher\n'
APP_MARKER = b'Omarchy Replay installed runtime v1\n'
BLOCKS = (
    ('-- BEGIN Omarchy Replay window', '-- END Omarchy Replay window', 'replay-window.lua'),
    ('-- BEGIN Omarchy Replay viewer exclusion', '-- END Omarchy Replay viewer exclusion', 'replay-viewer.lua'),
    ('-- BEGIN Omarchy Replay capture exclusions', '-- END Omarchy Replay capture exclusions', 'capture-exclusions.lua'),
)


def unit_text(binary):
    quoted = '"' + str(binary).replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('$', '$$') + '"'
    return ('[Unit]\nDescription=Replay screen history\nAfter=graphical-session.target\n'
            'PartOf=graphical-session.target\nStartLimitIntervalSec=60\nStartLimitBurst=3\n\n'
            '[Service]\nType=simple\n' + f'ExecStart={quoted} daemon run\n'
            'Restart=on-failure\nRestartSec=5\nTimeoutStopSec=55\nKillMode=control-group\n'
            'UMask=0077\nNice=10\nIOSchedulingClass=idle\n'
            'Environment=OMP_THREAD_LIMIT=1\nEnvironment=PYTHONDONTWRITEBYTECODE=1\n'
            'StandardOutput=null\nStandardError=journal\n\n[Install]\nWantedBy=graphical-session.target\n')


def regular_snapshot(path):
    """Never follow a managed-file symlink, including on rollback."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
        raise RuntimeError(f'Refusing a symlink, unowned, linked or non-regular managed file: {path}')
    return (path.read_bytes(), stat.S_IMODE(info.st_mode))


def owned_file(path, markers):
    old = regular_snapshot(path)
    if old is not None and not any(marker.encode() in old[0] for marker in markers):
        raise RuntimeError(f'Existing file is not managed by Replay: {path}')
    return old


def safe_parents(path, boundary):
    """Permit the user's chosen XDG root, but not redirects underneath it."""
    path.relative_to(boundary)
    for directory in [boundary, *reversed(list(path.parent.parents)[:len(path.parent.parts)-len(boundary.parts)]), path.parent]:
        if directory == boundary:
            continue
        if directory.exists() or directory.is_symlink():
            info = directory.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
                raise RuntimeError(f'Replay needs an owned directory without symlinks: {directory}')


class FileTransaction:
    def __init__(self):
        self.before = {}
        self.after = {}
        self.applied = []
        self.references = {}

    def plan(self, path, data, mode=0o600, markers=None):
        old = owned_file(path, markers) if markers else regular_snapshot(path)
        self.before[path] = old
        self.after[path] = None if data is None else (data, old[1] if old is not None else mode)

    def check(self):
        for path, old in self.before.items():
            if regular_snapshot(path) != old:
                raise RuntimeError(f'File changed while preparing Replay installation: {path}')

    def apply(self):
        self.check()
        for path, new in self.after.items():
            if regular_snapshot(path) != self.before[path]:
                raise RuntimeError(f'File changed during Replay installation: {path}')
            if new == self.before[path]:
                continue
            if new is None:
                path.unlink()
            else:
                atomic_write(path, *new)
            self.applied.append(path)

    def rollback(self):
        conflicts = []
        for path in reversed(self.applied):
            try:
                if regular_snapshot(path) != self.after[path]:
                    conflicts.append(str(path))
                    continue
                old = self.before[path]
                if old is None:
                    reference = self.references.get(path)
                    if reference and reference.exists() and lua_string(str(path)).encode() in reference.read_bytes():
                        conflicts.append(str(path) + ' remains referenced by desktop configuration')
                        continue
                    path.unlink(missing_ok=True)
                else:
                    atomic_write(path, *old)
            except (OSError, RuntimeError):
                conflicts.append(str(path))
        if conflicts:
            raise RuntimeError('Preserved concurrent edits; manual recovery needed: ' + ', '.join(conflicts))


def strip_block(original, begin, end):
    if begin not in original and end not in original:
        return original
    if original.count(begin) != 1 or original.count(end) != 1:
        raise RuntimeError('Replay configuration markers are ambiguous; no files changed.')
    before, rest = original.split(begin)
    if end not in rest:
        raise RuntimeError('Replay configuration markers are out of order; no files changed.')
    _, after = rest.split(end)
    return before.rstrip() + ('\n' if before.rstrip() else '') + after.lstrip('\n')


def shortcut_text(original, launcher, check_collision=True):
    if BEGIN not in original and check_collision:
        binds = json.loads(hyprctl('-j', 'binds'))
        if any(b.get('modmask') == 72 and str(b.get('key', '')).upper() == 'R' for b in binds):
            raise RuntimeError('Super+Alt+R is already bound; use --no-shortcut and choose your own binding.')
    base = strip_block(original, BEGIN, END)
    command = shlex.quote(str(launcher)) + ' open --toggle --notify-errors'
    if BEGIN in original:
        # Markers were validated above. Preserve the user's key/label when the
        # one managed binding already uses this installation's stable launcher.
        block = original.split(BEGIN, 1)[1].split(END, 1)[0].strip()
        lua_literal = r'"(?:[^"\\\n]|\\[^\n])*"'
        binding = r'o\.bind\(\s*' + lua_literal + r'\s*,\s*' + lua_literal + r'\s*,\s*' + re.escape(lua_string(command)) + r'\s*\)\s*;?'
        if re.fullmatch(binding, block):
            return original
    return base.rstrip() + '\n\n' + BEGIN + '\no.bind("SUPER + ALT + R", "Omarchy Replay", ' + lua_string(command) + ')\n' + END + '\n'


# Retained for source integrations and focused regression tests.
def managed_write(path, text, marker, mode):
    tx = FileTransaction(); tx.plan(path, text.encode(), mode, [marker]); tx.apply()


def install_shortcut(config):
    path = config / 'hypr/bindings.lua'
    old = regular_snapshot(path)
    if old is None:
        raise RuntimeError('Expected Omarchy Lua bindings; install with --no-shortcut to skip the shortcut.')
    text = shortcut_text(old[0].decode(), ROOT / 'scripts/replay')
    validate()
    if regular_snapshot(path) != old:
        raise RuntimeError('Bindings changed while preparing Replay shortcut; retry after reviewing the file.')
    tx = FileTransaction(); tx.plan(path, text.encode())
    try:
        tx.apply(); hyprctl('reload'); validate()
    except BaseException:
        tx.rollback(); hyprctl('reload'); raise


def systemctl(*args, check=True, timeout=10):
    return subprocess.run(['systemctl', '--user', *args], check=check, capture_output=True, text=True, timeout=timeout)


def service_state(unit):
    if not unit.exists():
        loaded = systemctl('show', 'omarchy-replay.service', '--property=FragmentPath', '--value', check=False).stdout.strip()
        if loaded:
            raise RuntimeError('A Replay unit outside the managed user path already exists.')
        return {'exists': False, 'active': False, 'enabled': False}
    owned_file(unit, [UNIT_MARKER])
    active = systemctl('is-active', '--quiet', 'omarchy-replay.service', check=False).returncode == 0
    enabled = systemctl('is-enabled', 'omarchy-replay.service', check=False).stdout.strip()
    if enabled not in ('enabled', 'disabled'):
        raise RuntimeError('Replay service enablement must be enabled or disabled before installation: ' + enabled)
    return {'exists': True, 'active': active, 'enabled': enabled == 'enabled'}


def restore_service(previous):
    systemctl('daemon-reload')
    if previous['exists']:
        systemctl('enable' if previous['enabled'] else 'disable', 'omarchy-replay.service')
        if previous['active']:
            systemctl('start', 'omarchy-replay.service')
    else:
        systemctl('reset-failed', 'omarchy-replay.service', check=False)


def paths():
    home = Path.home()
    config = xdg_path('XDG_CONFIG_HOME', home / '.config')
    data = xdg_path('XDG_DATA_HOME', home / '.local/share')
    state = xdg_path('XDG_STATE_HOME', home / '.local/state')
    result = dict(home=home, config=config, data=data, state=state,
                  app=data / 'omarchy-replay/app', unit=config / 'systemd/user/omarchy-replay.service',
                  launcher=home / '.local/bin/omarchy-replay', desktop=data / 'applications/omarchy-replay.desktop',
                  settings=config / 'omarchy-replay/config.toml', recording=state / 'omarchy-replay/recording.json',
                  hypr=config / 'hypr/hyprland.lua', bindings=config / 'hypr/bindings.lua',
                  runtime=xdg_path('XDG_RUNTIME_DIR', Path(tempfile.gettempdir()) / f'omarchy-replay-user-{os.getuid()}') / 'omarchy-replay')
    for path, boundary in [(result['unit'], config), (result['settings'], config), (result['hypr'], config),
                           (result['bindings'], config), (result['desktop'], data), (result['app'] / '.managed', data),
                           (result['launcher'], home), (result['recording'], state)]:
        safe_parents(path, boundary)
    for base in (config, data, state, xdg_path('XDG_CACHE_HOME', home / '.cache')):
        legacy, current = base / 'oma-rewind', base / 'omarchy-replay'
        if legacy.exists() and not (legacy.is_symlink() and legacy.resolve() == current.resolve()):
            raise RuntimeError('Unmigrated oma-rewind data exists. Run the previous source installer to migrate it first; no data was changed.')
    if (config / 'systemd/user/oma-rewind.service').exists():
        raise RuntimeError('Migrate the legacy oma-rewind.service with the previous installer first.')
    return result


@contextmanager
def install_lock(p):
    directory = p['state'] / 'omarchy-replay'
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(directory / 'installation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise RuntimeError('Invalid Replay installation lock.')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('Another Replay installation is in progress.') from error
        yield
    finally:
        os.close(descriptor)



class CoordinatorLease:
    """Use the native controller's lock inode; never unlink it."""
    def __init__(self, p):
        self.path = p['runtime'] / 'coordinator.lock'
        self.fd = None

    def acquire(self):
        directory = self.path.parent
        if directory.is_symlink():
            raise RuntimeError('Replay coordinator runtime is a symlink.')
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        metadata = directory.stat()
        if metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
            raise RuntimeError('Replay coordinator runtime must be private and owned by this user.')
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            metadata = os.fstat(self.fd)
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_nlink != 1 or metadata.st_mode & 0o077:
                raise RuntimeError('Replay coordinator lock is not a private owned file.')
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            self.release()
            raise RuntimeError('Replay coordinator is busy; no desktop integration was changed.')

    def release(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def has_owned_viewer(p):
    candidates = []
    for path in (ROOT / 'build/replay', ROOT / 'bin/replay'):
        if path.is_file():
            candidates.append(path)
    versions = p['app'] / 'versions'
    if versions.is_dir() and not versions.is_symlink():
        candidates += [item / 'bin/replay' for item in versions.iterdir() if item.is_dir() and not item.is_symlink()]
    hashes = None
    clients = json.loads(hyprctl('-j', 'clients'))
    for client in clients:
        if client.get('class') != 'omarchy-replay' and client.get('initialClass') != 'omarchy-replay':
            continue
        pid = client.get('pid')
        if type(pid) is not int or pid <= 0:
            continue
        try:
            executable = Path(f'/proc/{pid}/exe').resolve(strict=True)
            known = executable == ROOT / 'build/replay' or executable == ROOT / 'bin/replay'
            known = known or (executable.name == 'replay' and executable.parent.name == 'bin' and
                              executable.parent.parent.parent == p['app'] / 'versions')
            command = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if not known and executable.is_file() and executable.name == 'replay':
                if hashes is None:
                    hashes = set()
                    for path in candidates:
                        if path.is_file():
                            with path.open('rb') as stream:
                                hashes.add(hashlib.file_digest(stream, 'sha256').hexdigest())
                with executable.open('rb') as stream:
                    known = hashlib.file_digest(stream, 'sha256').hexdigest() in hashes
            if known and len(command) > 1 and command[1] == b'view':
                return True
        except (OSError, RuntimeError):
            continue
    return False


def preflight_runtime(binary):
    if sys.version_info < (3, 11):
        raise RuntimeError('Replay installation needs Python 3.11 or later.')
    missing = [name for name in ('python3', 'systemctl', 'hyprctl', 'tesseract') if not shutil.which(name)]
    if missing:
        raise RuntimeError('Missing Replay dependencies: ' + ', '.join(missing))
    subprocess.run([str(binary), '--version'], check=True, capture_output=True, timeout=10)
    subprocess.run([str(binary), 'view', '--version'], check=True, capture_output=True, timeout=10,
                   env=dict(os.environ, QT_QPA_PLATFORM='wayland'))
    languages = subprocess.run(['tesseract', '--list-langs'], check=True, capture_output=True, text=True, timeout=10)
    if 'eng' not in languages.stdout.splitlines():
        raise RuntimeError('Install tesseract-data-eng before installing Replay.')
    # Enumerates compositor protocol availability; does not capture pixels.
    subprocess.run([str(binary), 'outputs'], check=True, capture_output=True, timeout=10)
    validate()


def owned_directory(path):
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.getuid():
        raise RuntimeError(f'Replay needs an owned real directory, never a symlink: {path}')
    return True


def current_target(app):
    owned_directory(app / 'versions')
    pointer = app / 'current'
    if not pointer.exists() and not pointer.is_symlink():
        return None
    if not pointer.is_symlink():
        raise RuntimeError('Replay app/current is not a managed symbolic link.')
    target = pointer.resolve(strict=True)
    if target.parent != (app / 'versions').resolve():
        raise RuntimeError('Replay app/current points outside its managed versions.')
    from package_runtime import verify_runtime
    verify_runtime(target, existing=True)
    return os.readlink(pointer)


def replace_pointer(app, target, expected):
    pointer = app / 'current'
    actual = os.readlink(pointer) if pointer.is_symlink() else None
    if actual != expected or (pointer.exists() and not pointer.is_symlink()):
        raise RuntimeError('Replay current runtime changed during installation.')
    if target is None:
        pointer.unlink(missing_ok=True)
        return
    temporary = app / ('.current-' + os.urandom(8).hex())
    try:
        temporary.symlink_to(target, target_is_directory=True)
        temporary.replace(pointer)
    finally:
        temporary.unlink(missing_ok=True)


def app_preflight(p):
    app = p['app']
    if owned_directory(app):
        unexpected = {path.name for path in app.iterdir()} - {'.managed', 'current', 'versions'}
        if unexpected:
            raise RuntimeError('Unexpected files in Replay application directory; review them before continuing: ' + ', '.join(sorted(unexpected)))
        owned_directory(app / 'versions')
        if regular_snapshot(app / '.managed') != (APP_MARKER, 0o600):
            raise RuntimeError('Replay application directory is not managed by this installer.')
        return current_target(app)
    return None


def integration_plan(p, payload, args, remove=False):
    tx = FileTransaction()
    for path, markers in [(p['unit'], [UNIT_MARKER]), (p['launcher'], [LAUNCHER_MARKER]),
                          (p['desktop'], ['Name=Omarchy Replay\n', 'Name=Replay\n'])]:
        owned_file(path, markers)
    hypr = regular_snapshot(p['hypr'])
    if hypr is None:
        raise RuntimeError('An existing Omarchy Lua Hyprland configuration is required.')
    text = hypr[0].decode()
    for begin, end, name in BLOCKS:
        target = p['config'] / 'omarchy-replay/hypr' / name
        safe_parents(target, p['config'])
        owned_file(target, ['-- Managed by Omarchy Replay:'])
        tx.references[target] = p['hypr']
        text = strip_block(text, begin, end)
        if remove:
            tx.plan(target, None, markers=['-- Managed by Omarchy Replay:'])
        else:
            if name == 'capture-exclusions.lua':
                policy, _ = read_policy(p['settings'])
                instance = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE', '')
                if any(rule['address'] and rule['compositor_instance'] != instance for rule in policy['windows']):
                    raise RuntimeError('An excluded window belongs to an old desktop session; reselect it in Settings first.')
                rule, _, _ = render_rules(policy)
            else:
                rule = (payload / 'config/hypr' / name).read_bytes()
            tx.plan(target, rule, markers=['-- Managed by Omarchy Replay:'])
            text = updated_config(text, str(target), begin, end)
    tx.plan(p['hypr'], text.encode())
    bindings = regular_snapshot(p['bindings'])
    if remove:
        if bindings is not None:
            tx.plan(p['bindings'], strip_block(bindings[0].decode(), BEGIN, END).encode())
        for name in ('unit', 'desktop', 'launcher'):
            tx.plan(p[name], None)
    else:
        if not args.no_shortcut:
            if bindings is None:
                raise RuntimeError('Expected Omarchy Lua bindings; use --no-shortcut to skip the shortcut.')
            tx.plan(p['bindings'], shortcut_text(bindings[0].decode(), p['launcher']).encode())
        launcher = '#!/usr/bin/env bash\n' + LAUNCHER_MARKER + 'export PYTHONDONTWRITEBYTECODE=1\nexec ' + shlex.quote(str(p['app'] / 'current/scripts/replay')) + ' "$@"\n'
        tx.plan(p['launcher'], launcher.encode(), 0o755)
        tx.plan(p['desktop'], desktop_entry(p['launcher']).encode(), 0o644)
        tx.plan(p['unit'], (UNIT_MARKER + unit_text(p['app'] / 'current/bin/replay')).encode())
    return tx


def check_config(binary):
    result = subprocess.run([str(binary), 'daemon', 'paths'], check=True, capture_output=True, text=True, timeout=10)
    resolved = json.loads(result.stdout)
    if resolved.get('config_error') or resolved.get('using_last_valid_config'):
        raise RuntimeError('Fix Replay configuration before updating: ' + str(resolved.get('config_error', 'fallback configuration is active')))
    status = subprocess.run([str(binary), 'daemon', 'status'], check=True, capture_output=True, text=True, timeout=10)
    return json.loads(status.stdout)



def wait_for_service(binary, prior):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            pid = systemctl('show', 'omarchy-replay.service', '--property=MainPID', '--value', timeout=2).stdout.strip()
            result = subprocess.run([str(binary), 'daemon', 'status'], check=True, capture_output=True, text=True, timeout=2)
            state = json.loads(result.stdout)
            if pid.isdecimal() and int(pid) > 0 and state.get('running') and state.get('pid') == int(pid):
                if any(state.get(key, default) != prior.get(key, default)
                       for key, default in [('intent', 'stopped'), ('indexing_paused', False)]):
                    raise RuntimeError('Replay recording or indexing intent changed during upgrade.')
                return
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        time.sleep(0.1)
    raise RuntimeError('Updated Replay coordinator did not become ready with the expected systemd identity.')


def recover_install(tx, p, previous, lease, pointer_changed, previous_target, target):
    failures = []
    steps = []
    if not previous['exists']:
        steps.append(lambda: systemctl('disable', 'omarchy-replay.service', check=False))
    steps.append(tx.rollback)
    if pointer_changed:
        steps.append(lambda: replace_pointer(p['app'], previous_target, target))
    steps += [lambda: hyprctl('reload'), validate]
    for action in steps:
        try:
            action()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            failures.append(str(error))
    lease.release()
    # A concurrent edit or invalid restored desktop needs review before restarting capture.
    if not failures:
        try:
            restore_service(previous)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            failures.append(str(error))
    return failures


def install(p, args):
    from package_runtime import stage_runtime, verify_runtime
    previous_target = app_preflight(p)
    previous = service_state(p['unit'])
    # A separate temporary payload allows complete verification before the old recorder stops.
    with tempfile.TemporaryDirectory(prefix='omarchy-replay-install-') as temporary:
        payload = Path(temporary) / 'runtime'
        manifest = stage_runtime(ROOT, payload, binary=args.binary) if args.binary else stage_runtime(ROOT, payload)
        preflight_runtime(payload / 'bin/replay')
        fresh = not p['settings'].exists()
        if fresh:
            command = [str(payload / 'bin/replay'), 'daemon', 'init']
            if args.output:
                command += ['--output', args.output]
            subprocess.run(command, check=True, capture_output=True, timeout=10)
        else:
            regular_snapshot(p['settings'])
        status = check_config(payload / 'bin/replay')
        if status.get('running') and not previous['active']:
            raise RuntimeError('A Replay coordinator is running outside its managed service; stop it before installing.')
        tx = integration_plan(p, payload, args)
        settings = tomllib.loads(p['settings'].read_text())
        configured_login = settings.get('service', {}).get('login_startup', False)
        login = previous['enabled'] if previous['exists'] else configured_login
        if type(configured_login) is not bool:
            raise RuntimeError('service.login_startup must be boolean.')
        version = manifest['version'] + '-' + manifest['content_hash'][:12]
        if not re.fullmatch(r'[A-Za-z0-9._-]+', version):
            raise RuntimeError('Unsafe runtime version name.')
        app = p['app']; versions = app / 'versions'; destination = versions / version
        app.mkdir(parents=True, exist_ok=True, mode=0o700)
        if not (app / '.managed').exists():
            atomic_write(app / '.managed', APP_MARKER, 0o600)
        versions.mkdir(exist_ok=True, mode=0o700)
        if destination.exists():
            if verify_runtime(destination)['content_hash'] != manifest['content_hash']:
                raise RuntimeError('Installed version differs from staged Replay payload.')
        else:
            stage_runtime(payload, destination)
        target = 'versions/' + version
        pointer_changed = False
        lease = CoordinatorLease(p)
        try:
            tx.check()
            if previous['active']:
                systemctl('stop', 'omarchy-replay.service', timeout=60)
            lease.acquire()
            replace_pointer(app, target, previous_target); pointer_changed = True
            tx.apply()
            hyprctl('reload'); validate()
            systemctl('daemon-reload')
            systemctl('enable' if login else 'disable', 'omarchy-replay.service')
            lease.release()
            if previous['active']:
                systemctl('start', 'omarchy-replay.service')
                wait_for_service(p['app'] / 'current/bin/replay', status)
            return {'installed': True, 'version': manifest['version'], 'content_hash': manifest['content_hash'],
                    'runtime': str(destination), 'launcher': str(p['launcher']), 'unit': str(p['unit']),
                    'coordinator_restarted': previous['active'], 'login_startup': login, 'configured_login_startup': configured_login,
                    'shortcut': None if args.no_shortcut else 'Super+Alt+R', 'history_preserved': True}
        except BaseException as error:
            recovery = []
            if pointer_changed:
                try:
                    systemctl('stop', 'omarchy-replay.service', timeout=60)
                except (OSError, subprocess.SubprocessError) as failed:
                    recovery.append(str(failed))
            recovery.extend(recover_install(tx, p, previous, lease, pointer_changed, previous_target, target))
            if recovery:
                raise RuntimeError(str(error) + '; rollback needs attention: ' + '; '.join(recovery)) from error
            raise


def uninstall(p, args):
    from package_runtime import verify_runtime
    previous_target = app_preflight(p)
    previous = service_state(p['unit'])
    tx = integration_plan(p, None, args, remove=True)
    validate()
    if has_owned_viewer(p):
        raise RuntimeError('Close Replay windows before uninstalling; your saved history will be preserved.')
    # Verify every removable runtime before deleting anything; never follow arbitrary paths.
    versions = p['app'] / 'versions'
    installed = []
    if versions.exists():
        for item in versions.iterdir():
            if item.is_symlink() or not item.is_dir():
                raise RuntimeError('Unexpected file in Replay runtime versions; remove it manually after review.')
            verify_runtime(item, existing=True); installed.append(item)
    recording = regular_snapshot(p['recording'])
    saved = json.loads(recording[0]) if recording else {}
    if not isinstance(saved, dict):
        raise RuntimeError('Replay recording intent is invalid.')
    # Removal is an explicit stop. Retaining running intent would restart capture on reinstall/login.
    saved['intent'] = 'stopped'
    tx.plan(p['recording'], (json.dumps(saved, separators=(',', ':')) + '\n').encode())
    pointer_changed = False
    lease = CoordinatorLease(p)
    try:
        tx.check()
        if previous['active']:
            systemctl('stop', 'omarchy-replay.service', timeout=60)
            # Clean service shutdown may update counters in recording.json. Preserve those too.
            recording = regular_snapshot(p['recording'])
            saved = json.loads(recording[0]) if recording else {}
            saved['intent'] = 'stopped'
            tx.plan(p['recording'], (json.dumps(saved, separators=(',', ':')) + '\n').encode())
        lease.acquire()
        if previous['exists']:
            systemctl('disable', 'omarchy-replay.service')
        tx.apply()
        if previous_target is not None:
            replace_pointer(p['app'], None, previous_target); pointer_changed = True
        hyprctl('reload'); validate()
        systemctl('daemon-reload')
    except BaseException as error:
        recovery = recover_install(tx, p, previous, lease, pointer_changed, previous_target, None)
        if recovery:
            raise RuntimeError(str(error) + '; rollback needs attention: ' + '; '.join(recovery)) from error
        raise
    finally:
        lease.release()
    # Desktop removal is committed. Version deletion is safe, bounded to verified payloads.
    for item in installed:
        verify_runtime(item, existing=True); shutil.rmtree(item)
    if versions.exists():
        versions.rmdir()
    if p['app'].exists():
        # Preserve ownership if an unrelated file appeared after preflight.
        if {path.name for path in p['app'].iterdir()} != {'.managed'}:
            raise RuntimeError('Desktop integration was removed; unexpected runtime files were preserved with the installation marker.')
        (p['app'] / '.managed').unlink()
        try:
            p['app'].rmdir()
        except OSError:
            if owned_directory(p['app']) and not (p['app'] / '.managed').exists() and not (p['app'] / '.managed').is_symlink():
                atomic_write(p['app'] / '.managed', APP_MARKER, 0o600)
            raise
    return {'uninstalled': True, 'history_preserved': True, 'config_preserved': True,
            'recording_intent': 'stopped', 'login_startup_enabled': False,
            'note': 'History and settings remain. Reinstall requires an explicit Start recording.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, help='use a separately built native executable for source installation')
    parser.add_argument('--output', help='display selection for fresh configuration only')
    parser.add_argument('--no-shortcut', action='store_true')
    parser.add_argument('--uninstall', action='store_true', help='remove integration; preserve history and settings')
    args = parser.parse_args(argv)
    try:
        p = paths()
        with install_lock(p):
            result = uninstall(p, args) if args.uninstall else install(p, args)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f'Replay install: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
