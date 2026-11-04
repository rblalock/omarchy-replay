"""Launch an existing history, preserving trial settings and reusing its window."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from runtime_layout import native_binary

ROOT = Path(__file__).resolve().parents[1]
# Mirrors replay::ocrLanguagesProblem: names joined by +, at most 256 characters.
OCR_LANGUAGES = re.compile(r'(?:script/)?[A-Za-z0-9_]+(?:\+(?:script/)?[A-Za-z0-9_]+)*')


def view_command(binary, dataset, config):
    command = [str(binary), 'view', '--dir', str(dataset)]
    if not isinstance(config, dict):
        raise RuntimeError('Trial contains invalid indexing settings.')
    # An arbitrary explicit dataset has no saved trial policy to resume.
    if not config:
        return command
    scheduler = config.get('scheduler', 'fixed')
    if scheduler not in ('fixed', 'adaptive'):
        raise RuntimeError('Trial contains invalid scheduler settings.')
    mode = config.get('ocr_mode', 'incremental')
    if mode not in ('full', 'incremental', 'regions'):
        raise RuntimeError('Trial contains invalid OCR mode.')
    command += ['--index-while-viewing', '--scheduler', scheduler, '--ocr-mode', mode]
    settings = [('ocr_cpu_percent', 10, 1 if scheduler == 'adaptive' else 0, 100),
                ('ocr_max_wall_ms', 60000, 1, 60000),
                ('ocr_cpu_ceiling_percent', 0, 0, 100)]
    if scheduler == 'adaptive':
        settings += [('idle_seconds', 60, 1, 3600), ('idle_cpu_percent', 40, 1, 100),
                     ('request_cpu_percent', 30, 1, 100), ('pressure_cpu_percent', 10, 1, 100)]
    for key, default, low, high in settings:
        value = config.get(key, default)
        if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
            raise RuntimeError('Trial contains invalid indexing settings.')
        if key in ('ocr_cpu_percent', 'ocr_cpu_ceiling_percent') and 0 < value < 1:
            raise RuntimeError('Trial contains invalid indexing settings.')
        if key in ('ocr_max_wall_ms', 'idle_seconds') and int(value) != value:
            raise RuntimeError('Trial contains invalid integer indexing settings.')
        command += ['--' + key.replace('_', '-'), str(value)]
    languages = config.get('ocr_langs', 'eng')
    if not isinstance(languages, str) or len(languages) > 256 or not OCR_LANGUAGES.fullmatch(languages) or \
            any(name.lower() in ('osd', 'equ') for name in languages.split('+')):
        raise RuntimeError('Trial contains invalid OCR languages.')
    if languages != 'eng':
        command += ['--ocr-langs', languages]
    reuse = config.get('ocr_reuse', False)
    if type(reuse) is not bool:
        raise RuntimeError('Trial contains invalid OCR reuse setting.')
    if reuse:
        command.append('--ocr-reuse')
    return command


def service_command(binary, dataset, config):
    saved = Path(dataset) / '.index-service.json'
    if saved.exists() or saved.is_symlink():
        # The native controller validates the current service policy. Reopening
        # must not overwrite a later explicit setting with older trial defaults.
        return [str(binary), 'service', 'ensure', '--dir', str(dataset)]
    # Reuse the same strict saved-policy validation as manual viewer indexing.
    command = view_command(binary, dataset, config)
    if '--index-while-viewing' not in command:
        return None
    return [str(binary), 'service', 'ensure', '--dir', str(dataset), *command[5:]]


def ensure_index_service(binary, dataset, config):
    command = service_command(binary, dataset, config)
    if command is None:
        return None
    result = subprocess.run(command, capture_output=True, timeout=8, check=False)
    if result.returncode:
        message = result.stderr.decode(errors='replace').strip()[-1000:]
        raise RuntimeError('Background indexing could not start. ' + (message or 'Check the saved trial settings.'))
    try:
        receipt = json.loads(result.stdout)
        return receipt if isinstance(receipt, dict) else {}
    except ValueError:
        return {}


class QuietParser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError(message)


def process_view(pid, binary):
    """Match the actual executable and supported view argv, never a window title."""
    if type(pid) is not int or pid <= 0:
        return None
    expected = native_binary(ROOT) if Path(binary).resolve() == ROOT / 'scripts/replay' else Path(binary).resolve()
    process = Path('/proc') / str(pid)
    try:
        if (process / 'exe').resolve(strict=True) != expected:
            return None
        with (process / 'cmdline').open('rb') as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            return None
        argv = [os.fsdecode(part) for part in raw.rstrip(b'\0').split(b'\0')]
        parser = QuietParser(add_help=False, allow_abbrev=False)
        parser.add_argument('command', choices=('view',))
        parser.add_argument('-d', '--dir', required=True)
        parser.add_argument('--index-while-viewing', action='store_true')
        parser.add_argument('--settings', action='store_true')
        parser.add_argument('--no-ocr-reuse', action='store_true')
        parser.add_argument('--ocr-reuse', action='store_true')
        for option in ('scheduler', 'ocr-mode', 'ocr-data-path', 'ocr-max-height', 'ocr-cpu-percent',
                       'ocr-max-wall-ms', 'idle-seconds', 'idle-cpu-percent', 'request-cpu-percent',
                       'pressure-cpu-percent', 'ocr-cpu-ceiling-percent', 'ocr-langs'):
            parser.add_argument('--' + option)
        args = parser.parse_args(argv[1:])
        dataset = Path(args.dir)
        if not dataset.is_absolute():
            dataset = (process / 'cwd').resolve(strict=True) / dataset
        return dataset.resolve(), args.index_while_viewing
    except (OSError, RuntimeError, ValueError):
        return None


def focus_existing(dataset, binary, require_indexing=False, toggle=False):
    try:
        result = subprocess.run(['hyprctl', '-j', 'clients'], capture_output=True, timeout=1, check=False)
        if result.returncode or len(result.stdout) > 1024 * 1024:
            return False
        clients = json.loads(result.stdout)
        if not isinstance(clients, list):
            return False
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return False
    for client in clients:
        if not isinstance(client, dict) or not client.get('mapped'):
            continue
        address = client.get('address')
        if not isinstance(address, str) or not re.fullmatch(r'0x[0-9a-fA-F]+', address):
            continue
        existing = process_view(client.get('pid'), binary)
        if existing is None or existing[0] != dataset:
            continue
        if require_indexing and not existing[1]:
            raise RuntimeError('Close the existing Replay window and reopen this trial to resume indexing.')
        if toggle:
            try:
                active = subprocess.run(['hyprctl', '-j', 'activewindow'], capture_output=True, timeout=1, check=False)
                current = json.loads(active.stdout) if active.returncode == 0 and len(active.stdout) < 65536 else {}
                if current.get('address') == address and current.get('pid') == client.get('pid'):
                    closed = subprocess.run(['hyprctl', 'dispatch', f'hl.dsp.window.close({{ window = "address:{address}" }})'],
                                            capture_output=True, timeout=1, check=False)
                    if closed.returncode != 0:
                        raise RuntimeError('Replay could not dismiss its viewer.')
                    return True
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                raise RuntimeError('Replay could not check its focused viewer.') from error
        # Hyprland's Lua dispatch is current; retain compatibility with older releases.
        # Only the validated hexadecimal address enters the fixed Lua expression.
        commands = [
            ['hyprctl', 'dispatch', f'hl.dsp.focus({{ window = "address:{address}" }})'],
            ['hyprctl', 'dispatch', 'focuswindow', 'address:' + address],
        ]
        for command in commands:
            try:
                result = subprocess.run(command, capture_output=True, timeout=1, check=False)
                if result.returncode == 0:
                    return True
            except (OSError, subprocess.TimeoutExpired):
                continue
        raise RuntimeError('This history is already open, but Hyprland could not focus its window.')
    return False


def launch_viewer(binary, dataset, config, toggle=False, settings=False):
    dataset = Path(dataset).expanduser().resolve()
    if not (dataset / 'index.sqlite').is_file():
        raise RuntimeError('This history has no saved index yet. Finish a trial before opening it.')
    try:
        ensure_index_service(binary, dataset, config)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f'Replay: {error} Saved history will still open; press I to inspect indexing.', file=sys.stderr)
    # The independent service now owns indexing. A plain existing viewer is
    # sufficient, and closing it cannot stop background processing.
    command = [str(binary), 'view', '--dir', str(dataset)]
    if settings:
        command.append('--settings')
    focused = focus_existing(dataset, binary, toggle=True) if toggle and not settings else focus_existing(dataset, binary)
    if focused and not settings:
        return 0
    return subprocess.call(command, env=dict(os.environ, OMP_THREAD_LIMIT='1'))
