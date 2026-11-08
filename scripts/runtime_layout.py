"""Resolve source and installed runtimes without depending on a source checkout."""
import json
import os
from pathlib import Path
import sys

# Installed releases are immutable; imports must not add Python caches to them.
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.2.0'


def native_binary(root=ROOT):
    root = Path(root).resolve()
    if (root / 'runtime-manifest.json').exists() or (root / 'bin').exists():
        binary = root / 'bin/replay'
        message = 'Installed Replay executable is missing. Reinstall Omarchy Replay.'
    elif (root / 'CMakeLists.txt').is_file():
        binary = root / 'build/replay'
        message = 'Build Replay first: ./scripts/replay build'
    else:
        raise RuntimeError('This directory is not a Replay source checkout or installed runtime.')
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(message)
    return binary


def read_json(path):
    try:
        with Path(path).open('rb') as stream:
            data = stream.read(1024 * 1024 + 1)
        return None if len(data) > 1024 * 1024 else json.loads(data)
    except (OSError, ValueError):
        return None


def selected_trial(args):
    """Keep explicit legacy histories viewable without shipping the trial recorder."""
    if args.trial:
        trial = args.trial.expanduser().resolve()
    else:
        latest = read_json(args.runs_dir / 'latest.json')
        if not isinstance(latest, dict) or not isinstance(latest.get('trial_directory'), str):
            raise RuntimeError('No trial is recorded here yet.')
        trial = Path(latest['trial_directory']).resolve()
        if trial.parent != args.runs_dir:
            raise RuntimeError('The latest trial pointer is outside the trial directory.')
    if not isinstance(read_json(trial / 'trial.json'), dict):
        raise RuntimeError('This directory does not contain trial metadata.')
    return trial
