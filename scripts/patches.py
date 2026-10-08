"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# probe.c (BBPATCH2) reads each write into a 4096-byte buffer and fails startup on an empty
# or oversized entry: enforce the contract here, by name, instead of failing at launch.
PATCH_WRITE_LIMIT=4096
# Where the community patch set comes from. Not redistributed here (see .gitignore for why):
# every author credited in the file kept their rights, and shadps4-emu/ps4_cheats carries no
# LICENSE, so fetching is the user's step. Note that ps4_cheats is the origin of most of these
# entries, yet its current file is missing patch names this port looks up (Skip Intro among
# them), so a community collection is the reliable source; tools/fetch_patches.sh says the same.
PATCH_DB_HINT='https://github.com/GoldHEN'  # community patch collections (README: Mods and patches)
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09). Their patch lists
# follow shadps4-emu/ps4_cheats PATCHES/Bloodborne.xml of 2026-10-02 (older lists missed timesteps:
# messengers and loading screen pictures replayed their animations).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++'],'uncap':['Uncap FPS++']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
}
# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


class MissingPatchDatabase(Exception):
    """The community patch set is absent; the port cannot compile patches without it.

    Kept distinct from ValueError so main() can print the one remedy (fetch the file) instead
    of a generic failure line, and so a wrong-version file does not get the same message.
    """

    def __init__(self, path):
        self.path=path
        super().__init__(str(path))


def validate_patch_requirements(names, game):
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if 'Restore Debug Menu (READ NOTES)' in names:
        font = game / 'dvdroot_ps4/font'
        missing = [name for name in ('DbgFont14h.ccm', 'DbgFont14h.tpf')
                   if not (font / name).is_file() or (font / name).stat().st_size == 0]
        if missing:
            raise ValueError('Debug menu needs non-empty font files in '
                             f'{font}: {", ".join(missing)}. Install the fonts from '
                             'https://www.nexusmods.com/bloodborne/mods/253 first; '
                             'or disable debug_menu in bbport.ini')


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text().splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        if w<=0 or h<=0: raise ValueError(f'invalid render resolution {override!r}')
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly 1920x1080 (4K Performance) is indistinguishable from the game's UI
    # coordinate space, which the port's UI composition recognizes by that size.
    if render==OUTPUT_SIZE: render=(1916,1078)
    return render,out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line, name):
    kind,value=line.get('Type'),line.get('Value')
    if not kind or value is None:
        raise ValueError('patch line is missing Type or Value')
    if kind=='bytes': data=bytes.fromhex(value.replace(' ',''))
    elif kind in ('bytes16','bytes32','bytes64'):
        width=int(kind[5:])//8
        number=int(value,0)
        if not 0<=number<1<<width*8:
            raise ValueError(f'{kind} value {value!r} does not fit {width} bytes')
        data=number.to_bytes(width,'little')
    elif kind=='float32': data=struct.pack('<f',float(value))
    elif kind=='float64': data=struct.pack('<d',float(value))
    elif kind=='utf8': data=value.encode()+b'\0'
    elif kind=='utf16': data=value.encode('utf-16-le')+b'\0\0'
    else: raise ValueError(f'unsupported patch type {kind!r}')
    if not 0<len(data)<=PATCH_WRITE_LIMIT:
        raise ValueError(f'{name}: write is {len(data)} bytes; the loader accepts '
                         f'1..{PATCH_WRITE_LIMIT} per entry')
    return data


def compile_patches(xml, names, app_version, segments):
    # The patch database is community data we cannot redistribute (see .gitignore), so it is
    # absent from a fresh clone. Name the file and the way to get it instead of letting
    # ET.parse raise a bare FileNotFoundError behind a generic "patches failed".
    if not xml.is_file():
        raise MissingPatchDatabase(xml)
    if b'<!entity' in xml.read_bytes().lower():
        # xml.etree expands internal general entities, so a hostile patch file could hang the
        # launcher with an entity-expansion bomb. None of the community patch sets use them.
        raise ValueError(f'{xml.name} declares XML entities; refusing to parse it')
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing:
        # A wrong AppVer lands here too: the file parsed, the entries did not match. Say which.
        raise ValueError(f'patches not found for app version {app_version}: {missing}. '
                         f'{xml.name} must be the Bloodborne 1.09 patch set (Metadata AppVer='
                         f'{app_version!r}, AppElf="eboot.bin"). If it is another game\'s or '
                         f'another version\'s, fetch the right one.')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address') or '',0)-EBOOT_BASE
            data=encode(line,name)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
BLOODBORNE_IDS={'CUSA00207','CUSA00208','CUSA00900','CUSA01363','CUSA03173','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/**/*.xml.
    Subfolders are scanned too (shadPS4/, GoldHEN/ patch collections). key is
    "<relative file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.rglob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            if b'<!entity' in path.read_bytes().lower():
                # Same guard as compile_patches: xml.etree expands internal general entities,
                # so a hostile file could hang the launcher in an entity-expansion bomb.
                raise ValueError('declares XML entities; refusing to parse it')
            root=ET.parse(path).getroot()
        except (ET.ParseError,OSError,ValueError) as error:
            print(f'Patches: {path.relative_to(directory)}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.relative_to(directory)}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled. A broken config is reported and ignored."""
    settings={}
    if config and Path(config).is_file():
        try:
            settings=json.loads(Path(config).read_text())
            if not isinstance(settings,dict):
                raise ValueError(f'root is a {type(settings).__name__}, not an object')
        except (json.JSONDecodeError,UnicodeDecodeError,OSError,ValueError) as error:
            print(f'Patches: ignoring broken {config}: {error}',file=sys.stderr)
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments, built_writes=()):
    """Writes of the selected external patches. A patch is skipped whole when it has
    unsupported lines, lands outside the eboot, or writes bytes that differ from an already
    compiled write at the same address (the built-in patches and earlier externals win; the
    in-game settings and the FPS preset own their addresses). Identical bytes are allowed."""
    applied={}
    for offset,data in built_writes:
        for i,b in enumerate(data): applied[offset+i]=b
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            conflict=None
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line,key)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                for i,b in enumerate(data):
                    if offset+i in applied and applied[offset+i]!=b:
                        conflict=f'{offset+i+EBOOT_BASE:#x}'
                        break
                if conflict: break
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        if conflict:
            print(f'Patches: skipped {key}: writes {conflict} differently than an already '
                  'compiled patch (in-game settings, FPS preset, or another external patch '
                  'owns that address)',file=sys.stderr)
            continue
        writes+=ours
        for offset,data in ours:
            for i,b in enumerate(data): applied[offset+i]=b
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def compile_stamp(a):
    """Everything that decides patches.bin's bytes: the arguments, the file inputs, the
    external patch tree with their stats and this script. Font validation (game-dir) only
    guards the compile without changing the blob, so it stays out of the stamp."""
    def entry(path):
        try:
            st=path.stat()
        except OSError: # a data dir without bbport.ini is legal (read_settings tolerates it)
            return None
        return [os.path.normcase(str(path)),st.st_size,st.st_mtime_ns]
    files={'settings':entry(a.settings),'xml':entry(a.xml),'eboot':entry(a.out/'eboot.elf')}
    if a.patches_config and a.patches_config.is_file(): files['config']=entry(a.patches_config)
    tree=[[os.path.normcase(str(path)),path.stat().st_size,path.stat().st_mtime_ns]
          for path in sorted(a.patches_dir.rglob('*.xml'))] if a.patches_dir and a.patches_dir.is_dir() else []
    return {'args':[a.fps,a.extra,a.app_version,a.render_res,a.output_res],'files':files,
            'tree':tree,'script':entry(Path(__file__))}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    a=p.parse_args()
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    # Checked before anything reads it: compile_stamp() stats the file next, and a fresh clone
    # has no patch database at all. Failing here names the file; failing later would surface as
    # a WinError from an unrelated step.
    if not a.xml.is_file():
        raise MissingPatchDatabase(a.xml)
    # A compile takes well under a second but runs at every launch: reuse patches.bin while
    # the stamp matches. A failed compile leaves no stamp, so the next launch retries.
    stamp_path=a.out/'patches-stamp.json'
    try: previous=json.loads(stamp_path.read_text())
    except (OSError,ValueError): previous=None
    current=compile_stamp(a)
    if previous==current:
        print(f'Patches: reusing {a.out/"patches.bin"}')
        return
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution.
    ui=OUTPUT_SIZE
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones; conflicts (same address, different bytes) are skipped above.
        available=external_patches(a.patches_dir,a.app_version,a.xml)
        selected=external_selection(available,a.patches_config)
        print(f'Patches: {len(available)} external entries under {a.patches_dir}, {len(selected)} selected')
        writes+=compile_external(selected,segments,writes)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    stamp_path.write_text(json.dumps(current))
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    try:
        main()
    except MissingPatchDatabase as error:
        # The only failure with a single, complete remedy. Print it in full instead of a one-line
        # reason: the user meets this on the very first launch of a fresh clone.
        sys.exit(f'''patches failed: the community patch database is missing.

  {error.path} does not exist. Bloodborne cannot start without it: the frame-rate
  presets, the render-resolution presets and every effect switch in bbport.ini are
  looked up by patch name in this file.

  It is not in the repository because it is community data whose authors granted no
  redistribution rights. Put it at that path, from either place:

    1. a community patch collection
         {PATCH_DB_HINT}  (README: "Mods and patches")
       The Bloodborne 1.09 XML. This is the source that works.

    2. tools/fetch_patches.sh, as a starting point only
         bash tools/fetch_patches.sh
       It downloads upstream's Bloodborne.xml, but upstream is missing patch names this
       port looks up, so the script will tell you it is not usable as-is rather than
       install it. Compare its entry names against the ones reported below if you use it.

  The file must be the Bloodborne 1.09 set (Metadata AppVer="01.09",
  AppElf="eboot.bin"). Other community XML may be dropped into the same directory.''')
    except (ValueError, OSError, StopIteration, KeyError, IndexError) as error:
        sys.exit(f'patches failed: {error}')
