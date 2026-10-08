#!/usr/bin/env python3
"""Relocate community cheat JSONs into bbport's address space.

Community patch families share data through absolute low patch-space addresses:
the CUSA03023 set keeps an "attacker" slot at 0x4000 — a master mod writes it
(`mov [moffs64], rax`) and a One-Hit-Kill mod reads it (a rip-relative cmp) to
tell attacker from player. The patch authors assume the PS4 layout (eboot at
0x400000), where those references are consistent; under bbport the boot image
is mapped at LOW_MIN instead, so unrelocated references hit unmapped or
foreign memory (the fog-gate SIGSEGV class of crashes).

bbport provides a scratch page — the tail page after the boot image — and this
script rewrites the references into it:

  decode domain   VA = PS4_EBOOT_BASE (0x400000) + json offset   [authors' layout]
  scratch_base    = 0x0800000000 (LOW_MIN) + round_page(boot image size)
  target mapping  T < PS4_EBOOT_BASE  -> scratch_base + T        (engine scratch slot)
  rip-relative    a 4-byte window whose implied target T = base + offset + i + 4
                  + disp32 lands below PS4_EBOOT_BASE: disp32 is rewritten so the
                  target lands on scratch_base + T. Branch rel32 windows survive
                  naturally: their targets are hook addresses in the patch space
                  (well above PS4_EBOOT_BASE), so they are left untouched.
  moffs64         an 8-byte window preceded by an A0-A3 opcode byte (the only
                  absolute-memory encoding); rewritten to scratch_base + T.
  untouched       register-relative displacements (e.g. [rax+0xf8]): their implied
                  T stays above PS4_EBOOT_BASE for any realistic offset, and an
                  injected-code displacement below the base would mean an
                  84+ MiB negative struct offset, which does not occur.
  untouched       "off" bytes — they restore original game bytes whose own
                  references are position-correct already.

The assumed scratch base is recorded as the "bbport_scratch" top-level field and
verified by the engine (runtime_cheats.c): a stale out/ tree is rejected with a
diagnosis instead of crashing on a dead absolute address.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PS4_EBOOT_BASE = 0x400000
LOW_MIN = 0x0800000000
PAGE = 0x1000
SCRATCH_SIZE = 0x100000  # must match CHEAT_SCRATCH_SIZE in probe.c
MOFFS_OPS = {0xA0, 0xA1, 0xA2, 0xA3}


def round_page(size: int) -> int:
    return (size + PAGE - 1) & ~(PAGE - 1)


def s32(raw: int) -> int:
    return raw - 0x100000000 if raw >= 0x80000000 else raw


def relocate_bytes(hex_str: str, patch_offset: int, image_base: int, scratch_base: int,
                   what: str) -> str:
    """Rewrite low-address references in one patch byte string. Returns the new hex."""
    if not hex_str:
        return hex_str
    data = bytearray(bytes.fromhex(hex_str))
    out = bytearray(data)
    relocated = 0

    # moffs64: an 8-byte absolute preceded by an A0-A3 opcode (REX.W or none).
    for i in range(1, len(data) - 7):
        if data[i - 1] in MOFFS_OPS or (i >= 2 and data[i - 2] in MOFFS_OPS):
            target = int.from_bytes(data[i:i + 8], 'little')
            if 0 < target < SCRATCH_SIZE:
                out[i:i + 8] = (scratch_base + target).to_bytes(8, 'little')
                relocated += 1

    # rip-relative: a 4-byte window whose authors'-domain target lands below the base.
    for i in range(len(data) - 3):
        rip_after = PS4_EBOOT_BASE + patch_offset + i + 4
        target = s32(int.from_bytes(data[i:i + 4], 'little')) + rip_after
        if 0 < target < SCRATCH_SIZE:
            new_disp = (scratch_base + target) - (image_base + patch_offset + i + 4)
            if not -(1 << 31) <= new_disp < (1 << 31):
                raise SystemExit(
                    f'{what}: scratch {scratch_base + target:#x} outside disp32 of the '
                    f'cave at {patch_offset:#x} — the scratch page moved out of reach')
            out[i:i + 4] = (new_disp & 0xFFFFFFFF).to_bytes(4, 'little')
            relocated += 1

    if relocated:
        print(f'  {what}: {relocated} reference(s) -> scratch '
              f'{scratch_base:#x}..{scratch_base + PS4_EBOOT_BASE:#x}')
    return out.hex().upper()


def relocate_file(path: Path, image_base: int, scratch_base: int) -> tuple[dict, int]:
    doc = json.loads(path.read_text(encoding='utf-8'))
    changed = 0
    blocks = []
    if isinstance(doc.get('master'), dict):
        blocks.extend(doc['master'].get('memory', []))
    for mod in doc.get('mods', []):
        blocks.extend(mod.get('memory', []))
    for entry in blocks:
        offset = int(entry['offset'], 16)
        relocated = relocate_bytes(entry.get('on', ''), offset, image_base, scratch_base,
                                   f'{path.name} @{offset:x} on')
        if relocated != entry.get('on', ''):
            entry['on'] = relocated
            changed += 1
    doc['bbport_scratch'] = f'{scratch_base:#x}'
    return doc, changed


def main() -> int:
    ap = argparse.ArgumentParser(description='Relocate cheat jsons for bbport (see module doc).')
    ap.add_argument('--cheats-dir', default='cheats')
    ap.add_argument('--boot-image', default='out/boot-linked.bin')
    ap.add_argument('--out', default='out/cheats')
    args = ap.parse_args()

    boot = Path(args.boot_image)
    if not boot.is_file() or boot.stat().st_size < 64:
        print(f'relocate_cheats: {boot} missing — run the prepare step first', file=sys.stderr)
        return 1
    # The scratch base follows the *image* size, not the file size: a BBPROBE2 file is
    # [magic 8s][size entry ns nr ni flags 6Q][segment/import/reloc tables][image data],
    # and the loader allocates round_page(size) + one scratch page from that field.
    with boot.open('rb') as f:
        head = f.read(64)
    if head[:7] != b'BBPROBE':
        print(f'relocate_cheats: {boot} is not a BBPROBE image', file=sys.stderr)
        return 1
    image_size = int.from_bytes(head[8:16], 'little')
    scratch_base = LOW_MIN + round_page(image_size)

    src = Path(args.cheats_dir)
    dst = Path(args.out)
    dst.mkdir(parents=True, exist_ok=True)
    total = 0
    for path in sorted(src.glob('*.json')):
        doc, changed = relocate_file(path, LOW_MIN, scratch_base)
        (dst / path.name).write_text(json.dumps(doc, indent=1), encoding='utf-8')
        print(f'Cheats: {path.name}: {changed} entry(ies) relocated')
        total += changed
    if not total:
        print('Cheats: no low-address references found (nothing to relocate)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
