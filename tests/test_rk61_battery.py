#!/usr/bin/env python3
"""RK61 Plus RC battery-measurement tests, driven through the patched uCsim simulator.

Run from the repo root (inside `nix develop`, after building the RK61 firmware):

    meson compile -C build royalkludge-rk61plus_default_smk.hex
    python3 -m unittest discover -s tests   # or: python3 tests/test_rk61_battery.py

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

SIM = Sim(firmware=RK61_FIRMWARE)


def setUpModule():
    reason = SIM.available()
    if reason:
        raise unittest.SkipTest(reason)


def _expected(count):
    """The level/low_power user_battery.c derives from a count (level = count*8/100
    clamped to 7; low_power = level <= 1)."""
    level = min((count * 8) // 100, 7)
    return level, 1 if level <= 1 else 0


class TestRcBattery(unittest.TestCase):
    """Invoke user_battery_measure() with the P0.0 sense pin staged high, flip it
    low after a scripted number of count-loop turns, and check the predicted
    battery_level / low_power land in keyboard_state."""

    def _measure(self, flip_after):
        kb = Rk61Sim(firmware=RK61_FIRMWARE)
        try:
            return kb.battery_level_after(flip_after)
        finally:
            kb.close()

    def test_sense_low_immediately_is_empty(self):
        # Sense pin flips low before the first count: level 0, critically low.
        self.assertEqual(self._measure(0), _expected(0))

    def test_sense_held_quarter_window(self):
        self.assertEqual(self._measure(25), _expected(25))

    def test_sense_held_half_window(self):
        self.assertEqual(self._measure(50), _expected(50))

    def test_sense_held_full_window_is_full(self):
        # Sense pin never flips: the count window expires at 100 -> level 7.
        self.assertEqual(self._measure(100), _expected(100))


if __name__ == "__main__":
    unittest.main()
