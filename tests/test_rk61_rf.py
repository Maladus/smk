#!/usr/bin/env python3
"""RK61 Plus BK3632 RF link/pairing tests, driven through the patched uCsim
simulator with a test-side SPI slave.

The firmware bit-bangs SPI to a BK3632; `rk61_rf_support.RfSlave` emulates the
chip test-side, shifting canned status bytes back on MISO and capturing the
outgoing report/link/pairing frames. Assertions are on those frames and on the
keyboard_state / conn_mode the RF path drives.

Run from the repo root, after building the RK61 firmware:

    meson compile -C build royalkludge-rk61plus_default_smk.hex
    python3 -m unittest discover -s tests   # or: python3 tests/test_rk61_rf.py
"""

import os
import unittest
from pathlib import Path

from sim import Sim, REPO_ROOT
from rk61_rf_support import Rk61RfSim as Rk61Sim, RfSlave


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


# LNK_BT1..3 (src/keyboards/royalkludge-rk61plus/kbdef.h; SAFE_RANGE = QK_USER).
LNK_BT1, LNK_BT2, LNK_BT3 = 0x7E40, 0x7E41, 0x7E42

# rf_controller.c frame layout: magic, length-3, command, payload...
RF_MAGIC = 0xAA
RF_CMD_LINK = 0x01
RF_CMD_REPORT = 0x02
RF_CMD_USB_MODE = 0x06

RF_MODE_BT1 = 0x01
RF_MODE_BT2 = 0x02
RF_MODE_2_4G = 0x00


def rf_checksum(data):
    return (0x55 - sum(data)) & 0xFF


class RfTestCase(unittest.TestCase):
    """Boot the RK61 into BLE mode with the emulated BK3632 attached."""

    def setUp(self):
        self.kb = Rk61Sim(firmware=RK61_FIRMWARE)
        self.kb.boot()
        self.kb.set_band_24g(False)  # P5.6 high (B) = BLE
        self.kb.set_wired(False)     # P5.5 low = on (wireless)
        self.slave = RfSlave(self.kb)
        self.slave.install()

    def tearDown(self):
        self.slave.remove()
        self.kb.close()

    def link_frames(self):
        return [f for f in self.slave.frames if f[2] == RF_CMD_LINK]

    def report_frames(self):
        return [f for f in self.slave.frames if f[2] == RF_CMD_REPORT]

    def state_byte(self, offset):
        return self.kb.get_xram(self.kb._a("keyboard_state") + offset, 1)[0]

    def set_connected(self, connected, paired=0):
        """Stage keyboard_state.connected/paired, which now come only from the
        RF status reply (no boot-time fake)."""
        ks = self.kb._a("keyboard_state")
        self.kb.set_xram(ks + 4, [connected])
        self.kb.set_xram(ks + 5, [paired])


class TestLinkSelection(RfTestCase):
    def test_short_press_selects_bt_channel(self):
        # Select BT1 first so the next press is a channel change.
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)
        self.slave.frames.clear()

        self.kb.call_key(LNK_BT2, True, self.slave)
        self.kb.call_key(LNK_BT2, False, self.slave)

        links = self.link_frames()
        self.assertTrue(links, "expected a link-selection frame")
        self.assertEqual(links[-1][3], 0, "a short press must not request pairing")
        self.assertEqual(links[-1][4], RF_MODE_BT2)
        self.assertEqual(self.state_byte(1), RF_MODE_BT2,
                         "keyboard_state.rf_link should follow the selection")

    def test_ble_keys_disabled_on_24g(self):
        self.kb.set_band_24g(True)
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)

        self.assertEqual(self.slave.frames, [], "2.4G must disable the BLE keys")
        self.assertEqual(self.kb.get_sfr(0x82), 0,
                         "kb_process_record must consume the disabled key (false)")


class TestPairing(RfTestCase):
    def test_long_press_sends_pairing_command(self):
        # A ready, paired, not-yet-connected link so rf_set_link_pairing() returns
        # after the first poll (status1 bit3=connected=0, bit4=paired=1).
        self.slave.set_status(0x87, 0x10)

        self.kb.call_key(LNK_BT1, True, self.slave)  # press arms the hold timer
        self.slave.frames.clear()
        # Jump the hold counter to the threshold so one kb_update() pairs.
        self.kb.set_xram(self.kb._static("kb", "link_hold_ticks"), [0x60, 0xEA])  # 60000
        self.kb.cold_call(self.kb._a("kb_update"), slave=self.slave)

        pairing = [f for f in self.link_frames() if f[3] == 1]
        self.assertTrue(pairing, "a long press must request pairing")
        self.assertEqual(pairing[0][4], RF_MODE_BT1)

        self.assertEqual(self.state_byte(4), 0, "connected comes from the status reply")
        self.assertEqual(self.state_byte(5), 1, "paired comes from the status reply")


class TestUsbFallback(RfTestCase):
    def _toggle_to_usb(self):
        self.kb.mark_usb_configured()
        # A live, connected BT1 link (rf_link=1, connected, paired).
        self.slave.set_status(0x87, 0x38)
        # Select BT1, then let the supervisor read the status so the channel is
        # actually connected (connected is no longer set optimistically).
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)
        self.kb.cold_call(self.kb._a("kb_update"), slave=self.slave)
        self.slave.frames.clear()
        # Short-press the active, connected channel again: BLE off, USB on.
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)

    def test_short_press_on_active_connected_toggles_to_usb(self):
        self._toggle_to_usb()

        usb = [f for f in self.slave.frames if f[2] == RF_CMD_USB_MODE]
        self.assertTrue(usb, "expected the USB-mode command")
        self.assertEqual(usb[0][3], 1, "rf_cmd_06(1) disables the RF link")
        self.assertEqual(self.kb.get_xram(self.kb._static("kb", "conn_mode"), 1)[0], 1,
                         "conn_mode must switch to USB")

    def test_short_press_on_active_disconnected_toggles_to_usb(self):
        """A short press on the active channel must toggle back to USB even when
        no host is connected - otherwise a dropped link strands the user in BLE."""
        self.kb.mark_usb_configured()
        # BT1 selected and paired but NOT connected (status1 bit3 = 0).
        self.slave.set_status(0x87, 0x30)
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)
        self.kb.cold_call(self.kb._a("kb_update"), slave=self.slave)
        self.assertEqual(self.state_byte(4), 0, "link must not be connected")
        self.slave.frames.clear()
        # Short-press the active channel again: BLE off, USB on.
        self.kb.call_key(LNK_BT1, True, self.slave)
        self.kb.call_key(LNK_BT1, False, self.slave)

        usb = [f for f in self.slave.frames if f[2] == RF_CMD_USB_MODE]
        self.assertTrue(usb, "expected the USB-mode command")
        self.assertEqual(self.kb.get_xram(self.kb._static("kb", "conn_mode"), 1)[0], 1,
                         "a short press on the active channel must toggle to USB even if not connected")

    def test_report_does_not_use_rf_after_toggle(self):
        self._toggle_to_usb()
        self.slave.frames.clear()

        self.kb.set_xram(self.kb._a("keyboard_report"),
                         [0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.kb.cold_call(self.kb._a("send_keyboard_report"), slave=self.slave)

        self.assertEqual(self.report_frames(), [],
                         "after toggling to USB a report must not hit the RF link")


class TestReport(RfTestCase):
    def test_report_frame_carries_keys(self):
        # report_keyboard_t: mods, reserved, keys[6]. rf_send_report() drops the
        # reserved byte and sends mods + keys[0..4].
        self.set_connected(1)
        self.kb.set_xram(self.kb._a("keyboard_report"),
                         [0x00, 0x00, 0x04, 0x05, 0x06, 0x00, 0x00, 0x00])
        self.kb.cold_call(self.kb._a("send_keyboard_report"), slave=self.slave)

        reports = self.report_frames()
        self.assertTrue(reports, "expected an RF report frame")
        frame = reports[-1]
        self.assertEqual(len(frame), 32)
        self.assertEqual(frame[0:3], [RF_MAGIC, 0x1D, RF_CMD_REPORT])
        self.assertEqual(frame[3:9], [0x00, 0x04, 0x05, 0x06, 0x00, 0x00])
        self.assertEqual(frame[9], 0, "byte9 is 0 while a key is held")
        self.assertEqual(frame[31], rf_checksum(frame[:31]))


class TestStatusReply(RfTestCase):
    def test_status_reply_is_decoded(self):
        self.slave.set_status(0x87, 0x18)  # ready + battery, connected + paired
        buf = self.kb._a("keyboard_state")  # any 2-byte xram scratch buffer
        self.kb.cold_call(self.kb._a("rf_get_status"), slave=self.slave,
                          dpl=buf & 0xFF, dph=(buf >> 8) & 0xFF, b=0x00)

        self.assertEqual(self.kb.get_sfr(0x82), 1, "rf_get_status must return true")
        self.assertEqual(self.kb.get_xram(buf, 2), [0x87, 0x18],
                         "the two status bytes must reach the caller's buffer")


class TestBootFallback(RfTestCase):
    """connected/paired come only from the RF status reply: a cold boot with no
    live link must fall back to USB immediately instead of waiting out a poll."""

    def test_boot_does_not_fake_connected(self):
        self.assertEqual(self.state_byte(4), 0, "connected must not be faked at boot")
        self.assertEqual(self.state_byte(5), 0, "paired must not be faked at boot")

    def test_boot_report_uses_usb_without_rf_link(self):
        self.kb.mark_usb_configured()
        self.slave.frames.clear()
        self.kb.set_xram(self.kb._a("keyboard_report"),
                         [0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.kb.cold_call(self.kb._a("send_keyboard_report"), slave=self.slave)
        self.assertEqual(self.report_frames(), [],
                         "with no RF link the report must fall back to USB")

    def test_connected_link_routes_over_rf(self):
        self.slave.set_status(0x87, 0x38)  # ready + battery, BT1 connected+paired
        self.kb.cold_call(self.kb._a("kb_update"), slave=self.slave)
        self.assertEqual(self.state_byte(4), 1, "a live link must set connected")
        self.slave.frames.clear()
        self.kb.set_xram(self.kb._a("keyboard_report"),
                         [0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.kb.cold_call(self.kb._a("send_keyboard_report"), slave=self.slave)
        self.assertTrue(self.report_frames(), "a live link must route over RF")


class TestModeSwitch(RfTestCase):
    """The stock mode inputs (0x7C00): P5.5 = band, P5.6 low = wired/USB."""

    def _conn_mode(self):
        return self.kb.get_xram(self.kb._static("kb", "conn_mode"), 1)[0]

    def test_wired_input_selects_usb(self):
        self.kb.set_wired(True)
        self.kb.cold_call(self.kb._a("kb_update_switches"), slave=self.slave)
        self.assertEqual(self._conn_mode(), 1, "P5.6 low must select USB")

    def test_wireless_input_selects_rf(self):
        self.kb.set_wired(False)
        self.kb.cold_call(self.kb._a("kb_update_switches"), slave=self.slave)
        self.assertEqual(self._conn_mode(), 0, "P5.6 high must select RF")

    def test_2_4g_band_pins_the_24g_link(self):
        self.kb.set_band_24g(True)
        self.kb.set_wired(False)
        self.kb.cold_call(self.kb._a("kb_update_switches"), slave=self.slave)
        self.assertEqual(self._conn_mode(), 0, "2.4G band must select RF")
        self.assertEqual(self.state_byte(1), RF_MODE_2_4G,
                         "2.4G band must pin the 2.4G link")


if __name__ == "__main__":
    unittest.main()
