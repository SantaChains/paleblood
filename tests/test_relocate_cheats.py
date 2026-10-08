"""Golden tests for scripts/relocate_cheats.py against the real CUSA03023 family.

The master keeps an "attacker" slot at patch-space 0x4000 (mov [moffs64], rax)
and the One-Hit-Kill mod reads it with a rip-relative cmp; both references must
land on the same scratch address, while the position-relative jump-backs and
the original-game "off" bytes stay untouched.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))

from relocate_cheats import PS4_EBOOT_BASE, LOW_MIN, relocate_bytes

IMAGE_BASE = LOW_MIN
SCRATCH = LOW_MIN + 0x593B000  # an example: image tail page for a 93,538,364-byte image


class RelocateCheatsTests(unittest.TestCase):
    ONE_HIT_CAVE = ('39F00F4EF0534889FB4881EBF0000000483B1D9787B2FA7406'
                    '5BE9111C93FC5BE90D1C93FC')

    def test_one_hit_kill_rip_relocated_to_scratch(self):
        out = bytes.fromhex(relocate_bytes(self.ONE_HIT_CAVE, 0x50DB852,
                                           IMAGE_BASE, SCRATCH, 'test'))
        # The rip-relative cmp sits at cave offset 16 (7 bytes): its disp32 is at 19..22.
        disp = int.from_bytes(out[19:23], 'little', signed=True)
        rip_after = IMAGE_BASE + 0x50DB852 + 23
        self.assertEqual(rip_after + disp, SCRATCH + 0x4000,
                         'cmp must read the scratch slot at 0x4000')

    def test_master_moffs_relocated_to_same_slot(self):
        master = '48A300400000000000008B88F80000008988F80000008B88F800000041894C2418E981C081FC'
        out = bytes.fromhex(relocate_bytes(master, 0x50DB810, IMAGE_BASE, SCRATCH, 'test'))
        moffs = int.from_bytes(out[2:10], 'little')
        self.assertEqual(moffs, SCRATCH + 0x4000,
                         'master must write the same scratch slot the mod reads')

    def test_jump_backs_untouched(self):
        out_hex = relocate_bytes(self.ONE_HIT_CAVE, 0x50DB852, IMAGE_BASE, SCRATCH, 'test')
        original = bytes.fromhex(self.ONE_HIT_CAVE)
        out = bytes.fromhex(out_hex)
        # The two E9 rel32 jump-backs (offsets 26..30 and 32..36) keep their bytes.
        self.assertEqual(out[26:31], original[26:31])
        self.assertEqual(out[32:37], original[32:37])
        # The je/pop prologue is untouched too.
        self.assertEqual(out[:20], original[:20])

    def test_off_bytes_untouched(self):
        off = '39F00F4EF0'
        self.assertEqual(relocate_bytes(off, 0x1A0D47B, IMAGE_BASE, SCRATCH, 'test'), off)


if __name__ == '__main__':
    unittest.main()
