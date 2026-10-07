#!/usr/bin/env bash
# Downloads the community patch database (patches/Bloodborne.xml) that scripts/patches.py
# requires but the repository does not ship.
#
# Why it is fetched rather than vendored: the file is community data. Every author credited
# inside it (Kyo, illusion, auser1337, emoose, Lance McDonald, ...) kept their rights, and
# shadps4-emu/ps4_cheats carries no LICENSE, which means all rights reserved. Vendoring it
# would redistribute work the port has no permission to pass on.
#
# KNOWN LIMITATION, read before relying on this script. Upstream's current Bloodborne.xml does
# not satisfy the port on its own: patches.py resolves the frame-rate presets, the resolution
# presets and every bbport.ini effect switch by patch name, and upstream is missing some of
# those entries (Skip Intro among them). What works is upstream plus corrections this project
# carries, and those are not in the repository for the reason above. So this script validates
# what it downloaded and refuses to install a file the port cannot use, instead of leaving you
# with a file that fails later at a more confusing point. Treat a rejected download as "upstream
# is not sufficient", not as "the network is broken".
#
# Usage:
#   bash tools/fetch_patches.sh            # fetch patches/Bloodborne.xml if absent
#   bash tools/fetch_patches.sh --force    # re-fetch even if present (keeps a .bak)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

url='https://raw.githubusercontent.com/shadps4-emu/ps4_cheats/main/PATCHES/Bloodborne.xml'
dest=patches/Bloodborne.xml

force=0
for arg in "$@"; do
    case $arg in
        --force) force=1 ;;
        -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | cut -c3-; exit 0 ;;
        *) echo "usage: bash tools/fetch_patches.sh [--force]" >&2; exit 2 ;;
    esac
done

if [[ -s $dest && $force -eq 0 ]]; then
    echo "Patches: $dest already present ($(wc -c < "$dest") bytes); pass --force to re-fetch."
    exit 0
fi

mkdir -p "$(dirname -- "$dest")"

# Fetch beside the target and move into place, so an interrupted download never leaves a
# truncated XML that ET.parse would choke on with a confusing error.
tmp="$dest.part"
if ! curl -fsSL --retry 3 --retry-delay 1 -o "$tmp" "$url"; then
    rm -f "$tmp"
    echo "Patches: download failed: $url" >&2
    echo "  Check network access, or download the file by hand into $dest." >&2
    exit 1
fi

# A truncated or error page is worse than no file at all: verify it parses and really is
# the Bloodborne set before installing it.
if ! python - "$tmp" <<'PY'
import sys, xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except ET.ParseError as error:
    sys.exit(f'not valid XML: {error}')
meta = [m for m in root.iter('Metadata')]
if not meta:
    sys.exit('no <Metadata> entries; not a shadPS4 patch file')
names = {m.get('Name') for m in meta}
# The port's own lookups: FPS presets, the resolution template, effect switches, model LOD.
required = {'Uncap FPS++', 'Resolution Patch 1280x720 (16:9)', 'Skip Intro'}
absent = required - names
if absent:
    sys.exit(f'parsed, but missing entries this port needs: {sorted(absent)}')
if not any(m.get('AppVer') == '01.09' for m in meta):
    sys.exit('no Metadata with AppVer="01.09"; the port needs the 1.09 set')
PY
then
    rm -f "$tmp"
    echo "Patches: the downloaded file is not usable as-is; $dest left untouched." >&2
    echo "  Upstream's Bloodborne.xml does not contain every patch name this port looks up" >&2
    echo "  (see the KNOWN LIMITATION note at the top of this script). The set that works is" >&2
    echo "  upstream plus corrections this project carries, which are not redistributable." >&2
    echo "  Obtain the working set from a community patch collection instead:" >&2
    echo "    https://github.com/GoldHEN  (README: Mods and patches)" >&2
    exit 1
fi

# --force replaces the file, and the port's own copy is trimmed and corrected relative to
# upstream. Keep it, so re-fetching can never silently drop a local change.
if [[ -s $dest ]]; then
    mv "$dest" "$dest.bak"
    echo "Patches: existing $dest kept as $dest.bak"
fi
mv "$tmp" "$dest"
entries=$(grep -c '<Metadata ' "$dest" || true)
echo "Patches: $dest ($entries patch entries, $(wc -c < "$dest") bytes)"
echo "  Source: $url"
echo "  Review before enabling a patch: these are community edits to the game's code."