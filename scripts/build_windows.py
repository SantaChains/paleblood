#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows build entry (build.bat): build the port through MSYS2 (build.sh in the CLANG64
environment). Separate from run_windows.py so that starting the game never waits for a build.

    build.bat [--game-dir DIR] [--if-stale]

With --if-stale (run_windows.py's background build) nothing happens unless a source is
newer than bb-probe.exe.

The build is quiet on success; output goes to out\\build-launcher.log (an inherited IDE/runner
pipe can break MSYS stdout mid-build, so nothing is written to the console directly)."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def msys_root():
    """BB_MSYS2 from the environment, else from the user's registry environment (a parent
    process started before BB_MSYS2 was set, e.g. an IDE or an agent, carries a stale one)."""
    if os.environ.get('BB_MSYS2'):
        return Path(os.environ['BB_MSYS2'])
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Environment') as key:
            value, _ = winreg.QueryValueEx(key, 'BB_MSYS2')
            if value:
                return Path(value)
    except OSError:
        pass
    return Path(r'C:\msys64')


def if_stale():
    """True when a source is newer than bb-probe.exe — the staleness walk the launcher
    used to pay for at every start; here it runs in the background process instead."""
    exe = Path(os.environ.get('BB_DATA_DIR', ROOT)) / 'out' / 'bb-probe.exe'
    if not exe.is_file():
        return False  # nothing built yet: the launcher builds in the foreground
    mtime = exe.stat().st_mtime
    for base in ('src', 'gpu'):  # everything build.sh compiles into the port
        for dirpath, dirs, files in os.walk(ROOT / base):
            dirs[:] = [d for d in dirs if d != '.git']
            for name in files:
                if name.endswith(('.o', '.obj', '.a', '.lib', '.pdb', '.dll', '.exe')):
                    continue
                try:
                    if os.stat(os.path.join(dirpath, name)).st_mtime > mtime:
                        return True
                except OSError:
                    pass
    # Single-file link inputs of build.sh that live outside the walked trees.
    for name in ('build.sh', 'tools/gpu_capabilities.c', 'logo/bloodborne.ico'):
        try:
            if (ROOT / name).stat().st_mtime > mtime:
                return True
        except OSError:
            pass
    return False


def build():
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    out = Path(os.environ.get('BB_DATA_DIR', ROOT)) / 'out'
    out.mkdir(parents=True, exist_ok=True)
    log = out / 'build-launcher.log'
    with log.open('w', encoding='utf-8', errors='replace') as file:
        result = subprocess.run([str(bash), '-lc', 'bash build.sh'], cwd=ROOT, env=env,
                                stdout=file, stderr=subprocess.STDOUT)
    if result.returncode:
        print(f'build.sh failed, log tail (full: {log}):', file=sys.stderr)
        print(log.read_text(errors='replace')[-4000:], file=sys.stderr)
        sys.exit(result.returncode)
    print('Build OK (out/bb-probe.exe up to date)')


if __name__ == '__main__':
    if '--if-stale' in sys.argv:
        if not if_stale():
            sys.exit(0)
        print('Sources changed since the last build: rebuilding in the background '
              '(this launch runs the current build, the next one gets the new exe)')
    build()
