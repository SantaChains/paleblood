#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows launcher: prepare the game image, compile the patches and start
out/bb-probe.exe. Same environment variables and bbport.ini settings as run.bat.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
../CUSA03173. Building is separate (build.bat / build_windows.py, or run_windows.py
--build): the game starts directly, and the port is built only when bb-probe.exe is missing."""
import functools
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PYTHON = sys.executable

# The image chain (prepare/link_modules/content_profile) reads only the game's
# immutable binaries, the SKU and these scripts: its outputs are cached with a stamp of
# those inputs and rebuilt when one of them changes (about 17 s once, ~0 on relaunch).
# link_libc.py is not in the chain: boot-libc.bin feeds tests only, link_modules links
# libc.prx itself.
IMAGE_INPUTS = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                'sce_sys/param.sfo')
IMAGE_SCRIPTS = ('prepare.py', 'link_modules.py', 'content_profile.py')
IMAGE_OUTPUTS = ('boot-linked.bin', 'content.bin', 'eboot.elf')


def msys_root():
    """BB_MSYS2 from the environment, else from the user's registry environment (a parent
    process started before BB_MSYS2 was set, e.g. an IDE, carries a stale one)."""
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


def run(arguments, capture=False, check=True, env=None):
    result = subprocess.run([str(a) for a in arguments], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE if capture else None, text=True)
    if check and result.returncode:
        print(f'run_windows: step failed (exit {result.returncode}): '
              f'{shlex.join(str(a) for a in arguments)}', file=sys.stderr)
        if capture and result.stdout:
            print(result.stdout[-2000:], file=sys.stderr)
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config).
    Output goes to out\\build-launcher.log: the build is quiet on success, and an
    inherited IDE/runner pipe can break MSYS stdout mid-build (a silent exit 1)."""
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    log = Path(os.environ.get('BB_DATA_DIR', ROOT)) / 'out' / 'build-launcher.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8', errors='replace') as file:
        result = subprocess.run([str(bash), '-lc', 'bash build.sh'], cwd=ROOT, env=env,
                                stdout=file, stderr=subprocess.STDOUT)
    if result.returncode:
        print(f'build.sh failed, log tail (full: {log}):', file=sys.stderr)
        print(log.read_text(errors='replace')[-4000:], file=sys.stderr)
        sys.exit(result.returncode)


def background_build(out):
    """Rebuilds below normal priority in the background when a source is newer than
    bb-probe.exe: this launch starts the game right away on the current build, the next
    one picks up the new exe. The staleness walk runs in the build process (see
    build_windows.py --if-stale); launching never pays for it."""
    log = (out / 'build-background.log').open('w', encoding='utf-8', errors='replace')
    subprocess.Popen([sys.executable, str(SCRIPTS / 'build_windows.py'), '--if-stale'],
                     cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log,
                     stderr=subprocess.STDOUT,
                     creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)


def settings_value(config, key):
    return read_settings(Path(config)).get(key)


@functools.lru_cache(maxsize=8)
def read_settings(config: Path) -> dict:
    """bbport.ini parsed once per launch (it was re-read for every key before). Last
    duplicate key wins: the runtime's loader and patches.py read the file the same way."""
    settings = {}
    if config.is_file():
        for line in config.read_text(errors='replace').splitlines():
            name, _, value = line.partition('=')
            if name.strip():
                settings[name.strip()] = value.strip()
    return settings


def image_stamp(game, sku):
    """Fingerprint of everything the image chain reads: the game's binaries, the scripts, the SKU."""
    files = {}
    for name in IMAGE_INPUTS:
        st = (game / name).stat()
        files[name] = {'size': st.st_size, 'mtime_ns': st.st_mtime_ns}
    for name in IMAGE_SCRIPTS:
        st = (SCRIPTS / name).stat()
        files[f'scripts/{name}'] = {'size': st.st_size, 'mtime_ns': st.st_mtime_ns}
    return {'sku': sku, 'files': files}


def image_uptodate(game, out, sku):
    """True when out/boot-linked.bin was built from exactly these inputs."""
    try:
        if not all((out / name).is_file() for name in IMAGE_OUTPUTS):
            return False
        stamp = (out / 'image-stamp.json').read_text(encoding='utf-8')
        return json.loads(stamp) == image_stamp(game, sku)
    except (OSError, ValueError):
        return False


def main():
    # Console writes go through the legacy ANSI codepage (GBK) when stdout is a pipe, which
    # cannot encode characters like the trademark sign in game/mod file names.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    arguments = sys.argv[1:]
    want_build = '--build' in arguments
    arguments = [a for a in arguments if a != '--build']
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', ROOT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = Path(os.environ['BB_CONFIG'])
    # bbport.ini keys that feed the runtime through its BB_* environment variables
    # (language: PS4 system language, 11 = simplified Chinese; pad_swap: swap A/B and X/Y
    # for pads reporting the Nintendo layout; gc_budget_mb: texture cache budget;
    # gc_writeback: synchronous write-backs per GC pass under pressure;
    # present_mode: Mailbox (default) / Fifo (VSync, use with VRR) / Immediate);
    # fps: FPS patch preset, one of 30/60/90/uncap (default uncap; the in-game fps_cap
    # frame limiter is a separate key).
    # gc_writeback is the key bbport_settings.cpp parses; the older gc_downloads spelling is
    # still accepted below so an existing ini keeps working.
    for key, var in (('language', 'BB_LANGUAGE'), ('pad_swap', 'BB_PAD_SWAP'),
                     ('fps', 'BB_FPS'),
                     ('gc_budget_mb', 'BB_GC_BUDGET_MB'),
                     ('gc_writeback', 'BB_GC_DOWNLOADS_PER_PASS'),
                     ('gc_downloads', 'BB_GC_DOWNLOADS_PER_PASS'),
                     ('present_mode', 'BB_PRESENT_MODE')):
        value = settings_value(config, key)
        if value:
            os.environ.setdefault(var, value)
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    # The last folder that worked is remembered, so run.bat alone starts the game afterwards.
    remembered = out / 'game_dir.txt'
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else ROOT.parent / 'CUSA03173'
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)
    # The in-game menu's "Apply and restart" runs this launcher again (probe.c runtime_restart).
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    merged = Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    # Mods cannot replace the chain's inputs (eboot.bin, sce_module, sce_sys): they are
    # linked to the original files, so the image chain reads the original folder and its
    # outputs are stamped against it.
    sku = os.environ.get('BB_CONTENT_SKU', 'full')
    if image_uptodate(original, out, sku):
        pass
    else:
        for script, extra in (('prepare.py', []), ('link_modules.py', []),
                              ('content_profile.py', ['--sku', sku])):
            run([PYTHON, SCRIPTS / script, original, '--out', out, *extra])
        (out / 'image-stamp.json').write_text(json.dumps(image_stamp(original, sku)),
                                              encoding='utf-8')
    # Sizes chosen below for the previous launch are recomputed after an in-game restart.
    if os.environ.get('BB_AUTO_RENDER_RES') == '1':
        for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
            os.environ.pop(key, None)
    fps = os.environ.get('BB_FPS', 'uncap')
    scaled_render = scaled_output = None
    if not os.environ.get('BB_RENDER_RES'):
        # In process: patches.py --print-scaled was a whole interpreter start for one call.
        from patches import read_settings as patch_settings, scaled_sizes
        sizes = scaled_sizes(patch_settings(config))
        if sizes:
            scaled_render = f'{sizes[0][0]}x{sizes[0][1]}'
            scaled_output = f'{sizes[1][0]}x{sizes[1][1]}'
    # A normal launch never waits for a build: build.bat does the compiling on demand,
    # and stale sources rebuild in the background for the next launch.
    if want_build or not (out / 'bb-probe.exe').is_file():
        build()
    else:
        background_build(out)
    live = '0'
    if scaled_output:
        live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or '0'
        if live == 'auto':
            caps = out / 'bb-gpu-capabilities.exe'
            if caps.is_file():
                # check=False, conservative: the probe's stdout is only a resolution hint, so
                # a failing or odd run just falls back to the startup patch ("0") below.
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            else:
                print('bb-gpu-capabilities.exe is not built: auto live-resolution detection is '
                      'unavailable, using the startup patch instead')
                live = '0'
        live = '1' if live == '1' else '0'
    if live == '1':
        print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
    elif scaled_output:
        os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
        os.environ.setdefault('BB_DMEM_MB', '9152')
        print(f'Output {scaled_output}: scene {scaled_render}, direct memory {os.environ["BB_DMEM_MB"]} MiB '
              '(live_resolution=1: live changes)')
    run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
         '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
         '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
         '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
         '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
    os.environ.setdefault('BB_VBLANK_HZ', {'uncap': '0', '90': '90'}.get(fps, '60'))
    probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
    command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
               '--patches', out / 'patches.bin', '--app0', merged,
               '--user', os.environ.get('BB_USER_DIR', data / 'user'),
               '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
    # MSYS2's DLLs (libc++, SDL3, FFmpeg, ...). System32 is searched before PATH, so the
    # Vulkan loader stays the one installed with the GPU driver.
    os.environ['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), os.environ.get('PATH', '')])
    print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
    try:
        status = subprocess.call([str(c) for c in command], cwd=ROOT)
    except KeyboardInterrupt:
        status = 130
    return status


if __name__ == '__main__':
    sys.exit(main())
