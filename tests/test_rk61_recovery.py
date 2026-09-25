#!/usr/bin/env python3
"""RK61 Plus boot-time recovery tests, driven through the patched uCsim simulator.

Holding the top-left key (Esc, R0/C0) at power-on must hand the board to the ISP
bootloader before matrix_init() and USB come up, so a wedged image that never
reaches the host's feature report (or Fn+B) can still be reflashed.

Run from the repo root (inside `nix develop`, after building the RK61 firmware):

    meson compile -C build royalkludge-rk61plus_default_smk.hex
    python3 -m unittest discover -s tests   # or: python3 tests/test_rk61_recovery.py

Override targets with env vars SMK_UCSIM (simulator) and SMK_FIRMWARE (.hex).
"""

import os
import unittest
from pathlib import Path

from sim import Sim, REPO_ROOT
from devices import Rk61Sim, P7


def _rk61_firmware():
    env = os.environ.get("SMK_FIRMWARE")
    if env and Path(env).exists():
        return env
    return str(REPO_ROOT / "build" / "royalkludge-rk61plus_default_smk.hex")


RK61_FIRMWARE = _rk61_firmware()

SIM = Sim(firmware=RK61_FIRMWARE)

# isp_jump() does: clr EA; B=0xa5; A=0x5a; ljmp 0xff00.
ISP_ENTRY = 0xFF00
# Row R0 is P7.1; the board pulls it low while its column is driven and Esc is held.
ROW0_BIT = 0x02


def setUpModule():
    reason = SIM.available()
    if reason:
        raise unittest.SkipTest(reason)


class TestRk61Recovery(unittest.TestCase):
    def _reset_to_init(self):
        """Reset and patch the calibrated busy-wait delays to RET so init() runs
        without spinning for millions of sim cycles. The matrix pin staging must
        happen after this and before run(), because init() samples it."""
        kb = Rk61Sim(firmware=RK61_FIRMWARE)
        kb.cmd("reset")
        kb.cmd("set mem rom 0x%x 0x22" % kb._a("delay_us"))
        kb.cmd("set mem rom 0x%x 0x22" % kb._a("delay_ms"))
        return kb

    def test_esc_held_at_boot_jumps_to_isp(self):
        kb = self._reset_to_init()
        try:
            kb.set_pin(P7, 0xFF & ~ROW0_BIT)  # Esc held: row R0 pulled low
            kb.brk(ISP_ENTRY)
            out = kb.run()
        finally:
            kb.close()
        self.assertEqual(kb.stopped_at(out), ISP_ENTRY,
                         "held Esc at power-on should jump to the ISP bootloader")

    def test_esc_released_boots_normally(self):
        kb = self._reset_to_init()
        target = kb._a("kb_update_switches")
        try:
            kb.set_pin(P7, 0xFF)  # no key held
            kb.brk(ISP_ENTRY)
            kb.brk(target)
            out = kb.run()
        finally:
            kb.close()
        self.assertEqual(kb.stopped_at(out), target,
                         "released Esc must boot normally, not jump to ISP")


if __name__ == "__main__":
    unittest.main()
