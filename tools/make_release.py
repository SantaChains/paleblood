#!/usr/bin/env python3
"""Collects the runtime artifacts into a self-contained release zip.

A user who downloads the release needs none of the source tree: the executables are
statically linked (the only non-system DLL is SDL3), the Python launch chain uses the
standard library only, and every game-derived file (boot image, patch set, content
profile) is produced on the user's machine from their own game dump. Distributable
assets are shipped; licence-restricted ones (nvngx_dlss.dll) are fetched by the user
through tools/fetch_dlss.sh instead.

Usage: python tools/make_release.py [--out DIR] [--version V]
Writes out/release/bbport-<version>-win64.zip and prints its path.
"""
import argparse
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out'

# Files collected from the repository root, preserving their relative path.
ROOT_FILES = [
    'run.bat',
    'bbport.ini',
    'README.zh.md',
    'README.md',
    'RELEASE.zh.md',
    'THIRD-PARTY-LICENSES.md',
    'LICENSE',
]

# Files collected from out/ into the payload's own out/ subdirectory: the launch chain
# resolves its working outputs there (boot image, patches, overlay caches all land in
# out/ on the user's machine too), and the exe must sit next to out/SDL3.dll for loading.
BUILD_FILES = [
    'bb-probe.exe',
    'bb-gpu-capabilities.exe',
]

# Whole directories copied verbatim. fsr4_shaders is MIT-licensed model data (see
# fsr4_shaders/LICENSE-FSR4-v07.txt) and may be redistributed; patches/ is not (community
# data without redistribution rights — tools/fetch_patches.sh fetches it on the user's side).
COPY_DIRS = [
    ('scripts', '*.py'),
    ('fsr4_shaders', None),
    ('tools', None),
]


def find_sdl3() -> Path:
    """SDL3.dll: the one DLL the exe needs that Windows does not ship. Look next to the
    build outputs, then in the repository, then in the MSYS2 toolchain."""
    candidates = [
        OUT / 'SDL3.dll',
        ROOT / 'SDL3.dll',
    ]
    msys = Path(__import__('os').environ.get('BB_MSYS2', 'C:/msys64')) / 'clang64/bin/SDL3.dll'
    candidates.append(msys)
    # The build machine's MSYS2 may live elsewhere (the registry knows); try PATH last.
    for directory in __import__('os').environ.get('PATH', '').split(';'):
        if directory.strip():
            candidates.append(Path(directory.strip()) / 'SDL3.dll')
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit('SDL3.dll not found (looked in out/, the repo, BB_MSYS2 and PATH); '
                     'copy it next to bb-probe.exe and re-run')


def detect_version() -> str:
    """The release version comes from the VERSION file at the repository root — the same
    source the release workflow reads to decide whether to publish."""
    version_file = ROOT / 'VERSION'
    if version_file.is_file():
        version = version_file.read_text(encoding='utf-8').strip()
        if version:
            return version
    raise SystemExit(f'{version_file} is missing or empty (the release workflow reads it too)')


def collect(dest: Path) -> int:
    """Copies the release payload into dest; returns the file count."""
    dest.mkdir(parents=True, exist_ok=True)
    count = 0

    def put(src: Path, rel: str):
        nonlocal count
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        count += 1

    for name in ROOT_FILES:
        src = ROOT / name
        if not src.is_file():
            print(f'warning: missing {name}', file=sys.stderr)
            continue
        put(src, name)
    for name in BUILD_FILES:
        src = OUT / name
        if not src.is_file():
            raise SystemExit(f'{name} is missing — run build.bat first')
        put(src, f'out/{name}')
    put(find_sdl3(), 'out/SDL3.dll')
    for directory, pattern in COPY_DIRS:
        src_dir = ROOT / directory
        for src in sorted(src_dir.rglob(pattern or '*')):
            if src.is_file() and '__pycache__' not in src.parts:
                put(src, src.relative_to(ROOT).as_posix())
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=OUT / 'release')
    parser.add_argument('--version', default=detect_version())
    args = parser.parse_args()

    # The launch chain resolves scripts relative to the repository layout: scripts/ next to
    # the launcher, tools/ for the fetchers. Verify the inputs exist before zipping.
    staging = args.out / 'payload'
    if staging.exists():
        shutil.rmtree(staging)
    count = collect(staging)
    if not (staging / 'scripts' / 'run_windows.py').is_file():
        raise SystemExit('scripts/run_windows.py did not land in the payload')

    zip_path = args.out / f'bbport-{args.version}-win64.zip'
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for file in sorted(staging.rglob('*')):
            if file.is_file():
                z.write(file, file.relative_to(staging))
    size_mb = zip_path.stat().st_size / 1e6
    print(f'{zip_path} ({count} files, {size_mb:.1f} MB)')
    # Print the payload's exe imports as a sanity hint for the release notes.
    exe = staging / 'bb-probe.exe'
    if exe.is_file():
        head = exe.read_bytes()[:4]
        print('payload bb-probe.exe magic:', head[:2])
    return 0


if __name__ == '__main__':
    sys.exit(main())
