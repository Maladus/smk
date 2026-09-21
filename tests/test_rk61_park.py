#!/usr/bin/env python3
"""RK61 Plus boot/park firmware tests, driven through the patched uCsim simulator.

Run from the repo root (inside `nix develop`, after building the RK61 firmware):

    meson compile -C build royalkludge-rk61plus_default_smk.hex
    python3 -m unittest discover -s tests   # or: python3 tests/test_rk61_park.py

Override targets with env vars SMK_UCSIM (simulator) and SMK_FIRMWARE (.hex).
"""

import os
import unittest
from pathlib import Path

from sim import Sim, REPO_ROOT
from devices import Rk61Sim


def _rk61_firmware():
    env = os.environ.get("SMK_FIRMWARE")
    if env and Path(env).exists():
        return env
    return str(REPO_ROOT / "build" / "royalkludge-rk61plus_default_smk.hex")


RK61_FIRMWARE = _rk61_firmware()

# Sim() resolves firmware symbols only when the .hex exists; availability (the
# simulator binary) is checked against the RK61 firmware here so a missing
# nuphy-air60 build never masks an RK61 test.
SIM = Sim(firmware=RK61_FIRMWARE)


def setUpModule():
    reason = SIM.available()
    if reason:
        raise unittest.SkipTest(reason)


class TestBootValues(unittest.TestCase):
    """Boot through the real init path to the main loop, then check P0/P4/P7
    hold the stock boot values (`MOV P0,#24` / `MOV P4,#FD` / `MOV P7,#10`).
    P7 is read direction-aware by the simulator, so only its output bits are
    asserted: P7.4 (enable) high, P7.6 (control) low."""

    def test_boot_port_values(self):
        kb = Rk61Sim(firmware=RK61_FIRMWARE)
        try:
            kb.boot()
            p0, p4, p7 = kb.ports()
        finally:
            kb.close()
        self.assertEqual(p0, 0x24, f"P0 boot value; got 0x{p0:02x}")
        self.assertEqual(p4, 0xFD, f"P4 boot value; got 0x{p4:02x}")
        self.assertEqual(p7 & Rk61Sim.P7_OUT_BOOT, 0x10,
                         f"P7 output bits at boot (enable high); got 0x{p7:02x}")


class TestParkValues(unittest.TestCase):
    """After boot, invoke user_sleep_prepare() and check the enable group is
    driven low and the stock park masks are applied:
    `ANL P0,#1F/#FC` -> P0 0x04, `ANL P4,#92` -> P4 0x90, `ANL P7,#EE/#3F` ->
    every P7 output bit low."""

    def test_park_port_values(self):
        kb = Rk61Sim(firmware=RK61_FIRMWARE)
        try:
            kb.boot()
            p0, p4, p7 = kb.park()
        finally:
            kb.close()
        self.assertEqual(p0, 0x04, f"P0 park value; got 0x{p0:02x}")
        self.assertEqual(p4, 0x90, f"P4 park value; got 0x{p4:02x}")
        self.assertEqual(p7 & Rk61Sim.P7_OUT_PARK, 0x00,
                         f"P7 output bits after park (enable low); got 0x{p7:02x}")


if __name__ == "__main__":
    unittest.main()
