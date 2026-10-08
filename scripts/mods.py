#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import time

# Windows: symbolic links need administrator rights or developer mode. Directories become
# junctions and files hard links instead (a copy when the mod is on another volume).
WINDOWS = os.name == 'nt'

# Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself.
GAME_FOLDERS = {'action', 'chr', 'event', 'facegen', 'font', 'map', 'menu', 'movie', 'msg', 'mtd',
                'obj', 'other', 'param', 'paramdef', 'parts', 'remo', 'script', 'sfx', 'shader',
                'sound'}


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names). The fast path only takes an
    exact name: NTFS opens any casing, and the overlay keeps the game's spelling."""
    folder = Path(folder)
    direct = folder / name
    if direct.exists() and (not WINDOWS or
                            os.path.basename(os.path.realpath(direct)) == name):
        return direct
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


def content_root(folder):
    """(root, prefix): where a mod's files are and the game path they go to, or None.

    Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/CUSA03173/dvdroot_ps4,
    one wrapper folder around any of these (an archive extracted into a folder of its name), and
    the game's folders without dvdroot_ps4 (<mod>/chr, <mod>/parts, ...)."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    for wrapper in ('', 'app0', 'CUSA03173'):
        base = child(folder, wrapper) if wrapper else folder
        dvdroot = child(base, 'dvdroot_ps4')
        if dvdroot.is_dir():
            return base, ''
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if folders and all(e.name.casefold() in GAME_FOLDERS for e in folders):
        return folder, 'dvdroot_ps4'
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0])
    return None


def discover(root):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and content_root(p)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config):
    available = discover(root)
    if not config or not Path(config).is_file():
        return available
    settings = json.loads(Path(config).read_text())
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    disabled = set(disabled_names)
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    # New folders are enabled automatically and appended in alphabetical order.
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder):
    """(game path, source, stat) of every file a mod replaces or adds under dvdroot_ps4.
    One scandir pass per directory: size and mtime come straight from the scan, so the
    overlay fingerprint needs no second walk of the mod tree. Entries are sorted for a
    deterministic fingerprint."""
    layout = content_root(folder)
    if not layout:
        raise ValueError(f'{folder}: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder')
    root, prefix = layout
    root = root.resolve()

    def walk(directory):
        relative = Path(prefix) / directory.relative_to(root) if prefix else directory.relative_to(root)
        with os.scandir(directory) as scan:
            entries = list(scan)
        folders, files = [], []
        for entry in entries:
            # Junctions are directory symlinks under another name on Windows: is_symlink()
            # stays False for them, so they must be refused separately or a mod could pull
            # arbitrary local directories into the mounted game.
            if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
                raise ValueError(f'Mod symlinks and junctions are unsupported: {entry.path}')
            (folders if entry.is_dir(follow_symlinks=False) else files).append(entry)
        for entry in sorted(files, key=lambda e: e.name):
            source = Path(entry.path)
            relative_file = relative / entry.name
            # Readme/metadata stay outside the mounted game. This loader handles assets;
            # executable patches use the existing patch compiler, with address validation.
            if relative_file.parts[0].casefold() != 'dvdroot_ps4':
                if relative_file.parts[0].casefold() in ('eboot.bin', 'sce_module', 'sce_sys'):
                    raise ValueError(f'Executable/system replacement is unsupported: {source}')
                continue
            if not entry.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative_file, source, entry.stat()
        for entry in sorted(folders, key=lambda e: e.name):
            yield from walk(Path(entry.path))

    yield from walk(root)


def is_link(path):
    return path.is_symlink() or (WINDOWS and path.is_junction())


def link(path, target):
    """`path` refers to `target` (a directory or a file) without copying the game."""
    if not WINDOWS:
        path.symlink_to(target, target_is_directory=target.is_dir())
    elif target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(path))
    else:
        try:
            os.link(target, path)
        except OSError:
            shutil.copy2(target, path)


def unlink(path):
    """Removes a link made by `link` (never what it refers to)."""
    if WINDOWS and path.is_junction():
        os.rmdir(path)
        return
    try:
        path.unlink()
    except PermissionError:
        if not WINDOWS or is_link(path):
            raise
        os.chmod(path, stat.S_IWRITE)  # a read-only copy (hard links share the attribute)
        path.unlink()


def remove_overlay(path):
    """Deletes an overlay from build_overlay without following its links."""
    path = Path(path)
    if is_link(path) or not path.is_dir():
        if is_link(path) or path.exists():
            unlink(path)
        return
    for entry in path.iterdir():
        remove_overlay(entry)
    os.rmdir(path)


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if is_link(directory):
        target = directory.resolve(strict=True)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        unlink(directory)
        directory.mkdir()
        for entry in target.iterdir():
            link(directory / entry.name, entry)
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def sweep_stale_overlays(out):
    """Removes overlays no launch reached in a day: a killed session leaves its cache
    behind, and a fingerprint miss leaves superseded caches behind. The grace period
    keeps a concurrently running launch safe."""
    cutoff = time.time() - 24 * 3600
    for entry in Path(out).glob('mod-game-*'):
        try:
            if entry.is_dir() and entry.stat().st_mtime < cutoff:
                remove_overlay(entry)
        except OSError:
            pass


def touch(directory):
    """Marks a reused overlay as reached, so the day-based sweep does not collect a cache
    that is merely old — reusing it IS the activity the grace period exists to protect."""
    try:
        os.utime(directory, None)
    except OSError:
        pass


def fingerprint(game, mods, replacements):
    """Sha256 of everything the overlay's links point at: the game folder and its
    top-level entries, the ordered mod layers with their files (path, size, mtime)
    and this script. The overlay's directory name carries it, so a hit is never stale."""
    files = [[relative.as_posix(), str(source), st.st_size, st.st_mtime_ns]
             for relative, source, st in replacements]
    st = Path(__file__).stat()
    payload = {
        'game': str(game),
        'top': sorted((entry.name, entry.is_dir()) for entry in game.iterdir()),
        'layers': [[name, str(Path(root).resolve())] for name, root in mods],
        'files': files,
        'script': [st.st_size, st.st_mtime_ns],
    }
    return hashlib.sha256(json.dumps(payload, separators=(',', ':')).encode()).hexdigest()[:16]


def build_overlay(game, out, mods):
    game = Path(game).resolve(strict=True)
    replacements = []
    owners = {}
    for name, root in mods:
        count = 0
        for relative, source, st in mod_files(root):
            key = relative.as_posix().casefold()
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source, st))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    if not replacements:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Linking thousands of mod files takes tens of seconds: reuse the overlay built for
    # exactly these inputs (its name is the fingerprint, so a hit cannot be stale).
    fp = fingerprint(game, mods, replacements)
    cache = out / f'mod-game-{fp}'
    if cache.is_dir():
        print(f'Mods: reusing {cache.name}', file=sys.stderr)
        touch(cache)
        return cache
    # A rename can fail with the overlay finished (an antivirus holds the fresh directory for
    # a while on Windows); the fallback then keeps the mkdtemp-named directory. Carry the
    # fingerprint inside it so that build is still reusable instead of being redone — with
    # thousands of files plus a real-time scanner that is minutes of work per launch.
    for entry in Path(out).glob('mod-game-*'):
        marker = entry / '.fingerprint'
        try:
            if entry.is_dir() and marker.read_text(encoding='utf-8').strip() == fp:
                print(f'Mods: reusing {entry.name}', file=sys.stderr)
                touch(entry)
                return entry
        except OSError:
            pass
    sweep_stale_overlays(out)
    overlay = Path(tempfile.mkdtemp(prefix='mod-game-', dir=out))
    try:
        for entry in game.iterdir():
            link(overlay / entry.name, entry)
        replaced = added = 0
        # Progress every 500 files: a silent minutes-long stretch reads as a hang.
        for relative, source, _ in replacements:
            replaced_or_added = replaced + added
            if replaced_or_added and replaced_or_added % 500 == 0:
                print(f'Mods: linking {replaced_or_added}/{len(replacements)}...', file=sys.stderr)
            # Each component takes the game's spelling when it exists in another case.
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if destination.is_symlink():
                replaced += destination.resolve().is_relative_to(game)
                destination.unlink()
            elif WINDOWS and destination.is_file():
                # A hard link or copy: every file in the overlay is one of ours.
                replaced += 1
                unlink(destination)
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            link(destination, source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        # os.rename fails on Windows whenever the target exists (WinError 183), not just
        # while a process holds it — an earlier diagnosis as a purely transient antivirus
        # hold was wrong, and retries could never succeed. os.replace is the POSIX-style
        # atomic replace on both platforms; the retry only covers real transient holds
        # (an antivirus scanning the fresh directory).
        renamed = False
        for attempt in range(10):
            try:
                overlay.replace(cache)
                renamed = True
                break
            except OSError:
                if cache.is_dir():
                    break  # a concurrent launch finished the same overlay
                time.sleep(0.2)
        if not renamed:
            if cache.is_dir():
                remove_overlay(overlay)  # a concurrent launch finished the same overlay
                print(f'Mods: reusing {cache.name}', file=sys.stderr)
                return cache
            # Keep the finished overlay and carry the fingerprint inside it: the next
            # launch finds it through the marker even though the name is not the
            # fingerprint, so the minutes of linking are not thrown away.
            try:
                (overlay / '.fingerprint').write_text(fp, encoding='utf-8')
            except OSError:
                pass
            print(f'Mods: {overlay.name} kept (rename failed; reusable via fingerprint)', file=sys.stderr)
            return overlay
        return cache
    except BaseException:
        remove_overlay(overlay)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    args = parser.parse_args()
    layers, seen = [], set()

    def add(name, root):
        # One root, one layer: --mods-dir pointed at the legacy folder (or at a
        # link to it) would otherwise stack the same files twice under two names.
        key = os.path.normcase(str(Path(root).resolve()))
        if key not in seen:
            seen.add(key)
            layers.append((name, root))

    if args.enabled == '1':
        # shadPS4's loose overlay convention, beside the original game.
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir():
            add(legacy.name, legacy)
        # A directly selected loose overlay is also accepted.
        if child(args.mods_dir, 'dvdroot_ps4').is_dir():
            add(args.mods_dir.name, args.mods_dir)
        else:
            for name in selected(args.mods_dir, args.config):
                add(name, args.mods_dir / name)
    print(build_overlay(args.game, args.out, layers))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)
