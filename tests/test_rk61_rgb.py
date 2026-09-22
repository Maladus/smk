#!/usr/bin/env python3
"""RK61 Plus RGB matrix tests, driven through the patched uCsim simulator.

The LED engine runs inside the Timer2 tick ISR, interleaved with the matrix
scan, so the tests freeze interrupts (EA=0) and cold-invoke
indicators_start()/indicators_render()/indicators_update_step() against a
deterministic state. PWM duty is read back from the memory-mapped PWM window
(0xff80-0xfff9); the DUTY2 low/high byte planes sit at 0xffd0-0xffe1 and
0xffe8-0xfff9.

Run from the repo root, after building the RK61 firmware:

    meson compile -C build royalkludge-rk61plus_default_smk.hex
    python3 -m unittest discover -s tests   # or: python3 tests/test_rk61_rgb.py

Override targets with env vars SMK_UCSIM (simulator) and SMK_FIRMWARE (.hex).
"""

import os
import unittest
from pathlib import Path

from sim import Sim, REPO_ROOT
from devices import Rk61Sim, P4, P5, P6


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


# led_effect_t (src/smk/led_effect.h): FX_SOLID paints every key bright white.
FX_SOLID = 3
# FX_SOLID_RED is the RK61 default; FX_OFF (== FX_COUNT) is the end of the cycle.
FX_SOLID_RED = 4
FX_OFF = 5

# rf_mode_t (src/peripherals/bk3632/rf_controller.h), duplicated by the Fn
# channel indicator in indicators.c.
RF_BT1 = 0x01

# PWM DUTY2 register planes, part of the 0xff80-0xfff9 PWM window. 18 channels:
# PWM00-05, PWM10-15, PWM20-25; DUTY2 low bytes are 0xffd0 + i.
PWM_DUTY2L = 0xFFD0
PWM_DUTY2H = 0xFFE8

# DUTY2 index of each row's green/red/blue sink channel (channel order on the
# board is green, red, blue).
SINK = {
    0: (15, 16, 17),  # Esc row:   G=PWM23 R=PWM24 B=PWM25
    1: (6, 7, 8),     # Tab row:   G=PWM10 R=PWM11 B=PWM12
    2: (9, 10, 11),   # Caps row:  G=PWM13 R=PWM14 B=PWM15
    3: (3, 4, 5),     # Shift row: G=PWM03 R=PWM04 B=PWM05
    4: (0, 1, 2),     # Ctrl row:  G=PWM00 R=PWM01 B=PWM02
}
SPARE = (12, 13, 14)  # PWM20-22 (P3.0-2), the unconnected spare row

# The Fn+Q/W/E channel keys sit on the Tab row (row 1) at columns 1/2/3.
FN_ROW = 1
Q_COL, W_COL, E_COL = 1, 2, 3


class RgbSim:
    """A booted Rk61Sim frozen (EA=0) for deterministic LED-engine driving."""

    def __init__(self):
        self.kb = Rk61Sim(firmware=RK61_FIRMWARE)
        self.kb.boot()
        # Freeze the Timer2 tick so the LED engine only advances when the test
        # cold-invokes indicators_*.
        ie = self.kb.get_sfr(0xA8)
        self.kb.set_sfr(0xA8, ie & ~0x80)  # EA = 0
        # Reset the engine to a known state (led_col=0, counters zeroed).
        self.kb.call(self.kb._a("indicators_start"))

    def close(self):
        self.kb.close()

    def set_settings(self, effect, brightness=255, speed=4):
        # user_settings_t: led_effect, led_brightness, led_speed, ...
        self.kb.set_xram(self.kb._a("user_settings"), [effect, brightness, speed])

    def render(self):
        self.kb.set_xram(self.kb._static("indicators", "render_dirty"), [1])
        self.kb.call(self.kb._a("indicators_render"))

    def step(self):
        self.kb.call(self.kb._a("indicators_update_step"))

    def duty2l(self):
        return self.kb.get_xram(PWM_DUTY2L, 18)

    def duty2h(self):
        return self.kb.get_xram(PWM_DUTY2H, 18)

    def led_columns(self):
        """The 14 LED column levels (1 = driven HIGH) from the P6/P5/P4 latches."""
        p6 = self.kb.get_sfr(P6)
        p5 = self.kb.get_sfr(P5)
        p4 = self.kb.get_sfr(P4)
        cols = [(p6 >> c) & 1 for c in range(8)]   # C0-C7
        cols += [(p5 >> 0) & 1, (p5 >> 1) & 1,     # C8-C10
                 (p5 >> 2) & 1, (p5 >> 7) & 1]     # C10-C11
        cols += [(p4 >> 0) & 1, (p4 >> 2) & 1]     # C12-C13
        return cols

    def fb_rgb(self, row, col):
        """[R, G, B] framebuffer values for one key (led_fb[row][color][col])."""
        base = self.kb._static("indicators", "led_fb")
        return [self.kb.get_xram(base + row * 42 + color * 14 + col, 1)[0]
                for color in range(3)]

    def fb_blue(self, row, col):
        base = self.kb._static("indicators", "led_fb")
        return self.kb.get_xram(base + row * 42 + 2 * 14 + col, 1)[0]

    def stage_fn(self, rf_link, connected, paired, pairing_active, counter):
        """Stage the RF state the Fn channel indicator reads, then repaint."""
        ks = self.kb._a("keyboard_state")
        self.kb.set_xram(ks + 1, [rf_link])                # rf_link
        self.kb.set_xram(ks + 4, [connected])              # connected
        self.kb.set_xram(ks + 5, [paired])                 # paired
        self.kb.set_xram(self.kb._a("action_layer"), [1])  # Fn held
        self.kb.set_xram(self.kb._static("kb", "pairing_active"), [pairing_active])
        self.kb.set_xram(self.kb._static("indicators", "status_pulse_counter"), [counter])
        self.render()


class TestPwmDutyAndColumns(unittest.TestCase):
    """Drive the framebuffer through the engine and check the DUTY2 registers and
    the column GPIOs."""

    def test_solid_white_drives_every_sink_and_column(self):
        sim = RgbSim()
        try:
            sim.set_settings(FX_SOLID, brightness=255)
            sim.render()
            for col in range(14):
                sim.step()
                d2 = sim.duty2l()
                active = d2[:12] + d2[15:18]
                self.assertEqual(active, [255] * 15,
                                 f"col {col}: all 15 active sinks carry white")
                self.assertEqual(d2[12:15], [0, 0, 0],
                                 f"col {col}: spare P3.0-2 row stays dark")
                cols = sim.led_columns()
                if col == 9:
                    # P5.1 (C9) aliases TCON.1/IE0 in the simulator's base 8052
                    # model, which pins that bit high, so a byte read cannot
                    # observe C9's level. The PWM duty path above still covers it.
                    continue
                self.assertEqual(cols[col], 1, f"col {col}: selected column HIGH")
                for other in range(14):
                    if other != col and other != 9:
                        self.assertEqual(cols[other], 0,
                                         f"col {col}: column {other} stays LOW")
            self.assertEqual(sim.duty2h(), [0] * 18,
                             "DUTY2 high bytes stay zero for 8-bit duty")
        finally:
            sim.close()

    def test_colour_channels_map_to_green_red_blue_sinks(self):
        sim = RgbSim()
        try:
            base = sim.kb._static("indicators", "led_fb")
            # Paint column 0 with distinct per-row R/G/B values so any channel
            # swap is caught.
            for row in range(5):
                r, g, b = 0x20 + row, 0x60 + row, 0xA0 + row
                sim.kb.set_xram(base + row * 42 + 0 * 14 + 0, [r])
                sim.kb.set_xram(base + row * 42 + 1 * 14 + 0, [g])
                sim.kb.set_xram(base + row * 42 + 2 * 14 + 0, [b])

            sim.step()  # processes led_col == 0
            d2 = sim.duty2l()
            for row in range(5):
                gsink, rsink, bsink = SINK[row]
                self.assertEqual(d2[gsink], 0x60 + row,
                                 f"row {row}: green value lands on the G sink")
                self.assertEqual(d2[rsink], 0x20 + row,
                                 f"row {row}: red value lands on the R sink")
                self.assertEqual(d2[bsink], 0xA0 + row,
                                 f"row {row}: blue value lands on the B sink")
            for i in SPARE:
                self.assertEqual(d2[i], 0, f"spare sink PWM2{i - 10} stays dark")
        finally:
            sim.close()


class TestFnChannelIndicator(unittest.TestCase):
    """Fn-held Q/W/E channel indicator: solid when connected, slow blink while
    connecting, fast blink while pairing, and the inactive channel keys dark."""

    def test_solid_when_connected(self):
        sim = RgbSim()
        try:
            for counter in (0x00, 0x01, 0x08, 0x09):
                sim.stage_fn(RF_BT1, connected=1, paired=1,
                             pairing_active=0, counter=counter)
                self.assertEqual(sim.fb_blue(FN_ROW, Q_COL), 255,
                                 f"solid blue at counter 0x{counter:02x}")
                self.assertEqual(sim.fb_blue(FN_ROW, W_COL), 0, "W dark")
                self.assertEqual(sim.fb_blue(FN_ROW, E_COL), 0, "E dark")
                self.assertEqual(sim.fb_rgb(FN_ROW, Q_COL), [0, 0, 255],
                                 "active channel key is pure blue")
        finally:
            sim.close()

    def test_slow_blink_when_connecting(self):
        sim = RgbSim()
        try:
            for counter in (0x08, 0x18, 0x28):  # status_pulse_counter bit 3 set
                sim.stage_fn(RF_BT1, connected=0, paired=1,
                             pairing_active=0, counter=counter)
                self.assertEqual(sim.fb_blue(FN_ROW, Q_COL), 255,
                                 f"slow blink ON at counter 0x{counter:02x}")
                self.assertEqual(sim.fb_blue(FN_ROW, W_COL), 0, "W dark")
                self.assertEqual(sim.fb_blue(FN_ROW, E_COL), 0, "E dark")
            for counter in (0x00, 0x07, 0x10):  # bit 3 clear
                sim.stage_fn(RF_BT1, connected=0, paired=1,
                             pairing_active=0, counter=counter)
                self.assertEqual(sim.fb_blue(FN_ROW, Q_COL), 0,
                                 f"slow blink OFF at counter 0x{counter:02x}")
                self.assertEqual(sim.fb_blue(FN_ROW, W_COL), 0, "W dark")
                self.assertEqual(sim.fb_blue(FN_ROW, E_COL), 0, "E dark")
        finally:
            sim.close()

    def test_fast_blink_when_pairing(self):
        sim = RgbSim()
        try:
            for counter in (0x01, 0x03, 0x05):  # status_pulse_counter bit 0 set
                sim.stage_fn(RF_BT1, connected=0, paired=1,
                             pairing_active=1, counter=counter)
                self.assertEqual(sim.fb_blue(FN_ROW, Q_COL), 255,
                                 f"fast blink ON at counter 0x{counter:02x}")
                self.assertEqual(sim.fb_blue(FN_ROW, W_COL), 0, "W dark")
                self.assertEqual(sim.fb_blue(FN_ROW, E_COL), 0, "E dark")
            for counter in (0x00, 0x02, 0x04):  # bit 0 clear
                sim.stage_fn(RF_BT1, connected=0, paired=1,
                             pairing_active=1, counter=counter)
                self.assertEqual(sim.fb_blue(FN_ROW, Q_COL), 0,
                                 f"fast blink OFF at counter 0x{counter:02x}")
                self.assertEqual(sim.fb_blue(FN_ROW, W_COL), 0, "W dark")
                self.assertEqual(sim.fb_blue(FN_ROW, E_COL), 0, "E dark")
        finally:
            sim.close()

    def test_active_blue_reaches_the_q_pwm_sink(self):
        sim = RgbSim()
        try:
            sim.set_settings(FX_OFF, brightness=255)  # isolate the overlay
            sim.stage_fn(RF_BT1, connected=1, paired=1, pairing_active=0, counter=0)
            sim.kb.set_xram(sim.kb._static("indicators", "led_col"), [Q_COL])
            sim.step()  # processes the Q column
            d2 = sim.duty2l()
            # Q is row 1 (Tab); its blue sink is PWM12, DUTY2 index 8.
            self.assertEqual(d2[8], 255, "Q blue reaches the PWM12 sink")
            self.assertEqual(d2[:8] + d2[9:], [0] * 17,
                             "no other sink is lit for the Q column")
        finally:
            sim.close()

    def test_effect_stays_on_non_channel_keys_while_fn_held(self):
        """Holding Fn overlays the channel key; it must not darken the rest of
        the matrix, the effect keeps running underneath."""
        sim = RgbSim()
        try:
            sim.set_settings(FX_SOLID_RED, brightness=255)
            sim.stage_fn(RF_BT1, connected=1, paired=1, pairing_active=0, counter=0)
            self.assertEqual(sim.fb_rgb(0, 0), [255, 0, 0],
                             "Esc keeps the effect while Fn is held")
            self.assertEqual(sim.fb_rgb(FN_ROW, Q_COL), [0, 0, 255],
                             "the active channel key is overlaid blue")
        finally:
            sim.close()


class TestEffectControls(unittest.TestCase):
    """The Fn effect/brightness/speed controls mutate user_settings and clamp at
    the ends. The functions are cold-invoked directly, like the engine tests."""

    def _settings(self, sim):
        return sim.kb.get_xram(sim.kb._a("user_settings"), 3)

    def test_next_effect_cycles_and_wraps(self):
        sim = RgbSim()
        try:
            sim.kb.set_xram(sim.kb._a("user_settings"), [FX_SOLID_RED, 255, 4])
            sim.kb.call(sim.kb._a("indicators_next_effect"))
            self.assertEqual(self._settings(sim)[0], FX_OFF)
            sim.kb.call(sim.kb._a("indicators_next_effect"))
            self.assertEqual(self._settings(sim)[0], 0, "wraps back to the first effect")
        finally:
            sim.close()

    def test_prev_effect_wraps_to_off(self):
        sim = RgbSim()
        try:
            sim.kb.set_xram(sim.kb._a("user_settings"), [0, 255, 4])
            sim.kb.call(sim.kb._a("indicators_prev_effect"))
            self.assertEqual(self._settings(sim)[0], FX_OFF)
        finally:
            sim.close()

    def test_brightness_clamps_at_both_ends(self):
        sim = RgbSim()
        try:
            sim.kb.set_xram(sim.kb._a("user_settings"), [FX_SOLID_RED, 255, 4])
            sim.kb.call(sim.kb._a("indicators_brightness_up"))
            self.assertEqual(self._settings(sim)[1], 255)
            sim.kb.set_xram(sim.kb._a("user_settings") + 1, [0])
            sim.kb.call(sim.kb._a("indicators_brightness_down"))
            self.assertEqual(self._settings(sim)[1], 0)
        finally:
            sim.close()

    def test_speed_clamps_at_both_ends(self):
        sim = RgbSim()
        try:
            sim.kb.set_xram(sim.kb._a("user_settings"), [FX_SOLID_RED, 255, 16])
            sim.kb.call(sim.kb._a("indicators_speed_up"))
            self.assertEqual(self._settings(sim)[2], 16)
            sim.kb.set_xram(sim.kb._a("user_settings") + 2, [1])
            sim.kb.call(sim.kb._a("indicators_speed_down"))
            self.assertEqual(self._settings(sim)[2], 1)
        finally:
            sim.close()


if __name__ == "__main__":
    unittest.main()
