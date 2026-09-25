#!/usr/bin/env python3
"""Phase A Vial validation tests, driven through the patched uCsim simulator.

Run from the repo root (inside `nix develop`, after building the vial firmware):

    meson compile -C build royalkludge-rk61plus_vial_smk.hex
    python3 -m unittest tests.test_vial          # or the whole discover

Override the target with SMK_FIRMWARE (must be a `*_vial_smk.hex`).

The tests cover the plan's Phase A acceptance surface:
  * USB descriptors: interface 0 raw HID (0xFF60/0x61), interface 1 boot
    keyboard with feature report 5, no NKRO report id, the Vial serial magic.
  * ISP feature report 5 on the vial image still jumps to the bootloader.
  * VIA/Vial protocol replies (version, keyboard id, layer count, get_def,
    QMK settings, unhandled commands) and the EP0 SET_REPORT -> EP2 IN
    transport, including an unknown feature report not blocking the next Vial
    message.
  * Dynamic keymap get/set/reset and persistence, key resolution through the
    store.
  * Layer engine MO/DF/PDF/TG/TO, transparent fallthrough, no stuck key across
    a layer change, DF not persisting and PDF persisting, out-of-range clamping.
  * Tap/hold: tap under term, hold past term, permissive hold, a rolled event
    not lost, the QMK term changing the result, settings reset.
  * EP1 keyboard report size in report protocol (id + 6KRO) vs boot protocol.
"""

import functools
import lzma
import os
import re
import unittest
from pathlib import Path

from sim import (
    Sim, REPO_ROOT, load_symbols, read_intel_hex,
    get_descriptor, DESC_DEVICE, DESC_CONFIGURATION, DESC_STRING,
    set_report_isp_setup, set_report_output,
)
from devices import Rk61Sim
from rk61_rf_support import Rk61RfSim, RfSlave, RF_MAGIC, RF_CMD_REPORT


def find_vial_firmware():
    """The built `*_vial_smk.hex`, or None. `SMK_FIRMWARE` wins only when it is
    itself a vial image, so a global non-vial override does not mask this."""
    env = os.environ.get("SMK_FIRMWARE")
    if env and Path(env).exists() and "vial" in Path(env).name:
        return env
    candidates = (
        sorted(Path.cwd().glob("*vial*_smk.hex"))
        + sorted(REPO_ROOT.glob("build/*vial*_smk.hex"))
        + sorted(REPO_ROOT.glob("*vial*_smk.hex"))
    )
    return str(candidates[0]) if candidates else None


VIAL_FIRMWARE = find_vial_firmware()

# RK61 Plus matrix wiring: Fn sits at (row 4, col 13), Space at (4, 5),
# RAlt at (4, 8), Esc at (0, 0), '1' at (0, 1). Positions come from the
# LAYOUT_60 macro in layouts/default/layout.c.
FN_ROW, FN_COL = 4, 13
SPC_ROW, SPC_COL = 4, 5
RALT_ROW, RALT_COL = 4, 8
ESC_ROW, ESC_COL = 0, 0

MATRIX_COLS = 14

KC_A = 0x0004
KC_1 = 0x001E
KC_ESC = 0x0029
KC_SPC = 0x002C
KC_GRV = 0x0035
KC_LEFT = 0x0050

QK_TO = 0x5200
QK_DF = 0x5240
QK_TG = 0x5260
QK_PDF = 0x52E0
QK_LAYER_TAP = 0x4000

REPORT_ID_KEYBOARD = 4
RAW_HID_REPORT_SIZE = 32
EP1_BUF_SIZE = 16
EP2_BUF_SIZE = 64

# The two 512-byte keymap sectors (VIAL_KEYMAP_ADDR = settings - 2*sector).
# Only needed to invalidate the store when a test wants the seeded defaults.
KEYMAP_SECTORS = (0xE800, 0xEA00)

# Simulator SFR / sled addresses (see tests/sim.py).
USBIF1, USBIF2 = 0x92, 0x93
SETUPIF = 0x10
OEP0IF = 0x10
EP0_OUT_BUF = 0x1100
SLED = 0x9000
SLED_END = 0x900E

USB_DESC_CLASS_HID = 0x21
USB_DESC_CLASS_REPORT = 0x22


def setUpModule():
    if not VIAL_FIRMWARE:
        raise unittest.SkipTest(
            "no vial firmware hex; build it: meson compile -C build royalkludge-rk61plus_vial_smk.hex"
        )
    if not Path(VIAL_FIRMWARE).with_suffix(".map").exists():
        raise unittest.SkipTest(f"vial .map missing next to {VIAL_FIRMWARE}")
    reason = Sim(firmware=VIAL_FIRMWARE).available()
    if reason:
        raise unittest.SkipTest(reason)


# --- HID report descriptor parsing ----------------------------------------


def parse_hid_items(desc):
    """Decode HID short items into (bType, bTag, data, size) tuples.

    bType: 0 main, 1 global, 2 local. Raises ValueError on a truncated item so
    a malformed descriptor fails the test rather than silently under-parsing."""
    items = []
    i = 0
    while i < len(desc):
        prefix = desc[i]
        i += 1
        size = prefix & 0x03
        if size == 3:
            size = 4
        tag = (prefix >> 4) & 0x0F
        btype = (prefix >> 2) & 0x03
        if i + size > len(desc):
            raise ValueError(f"truncated HID item at offset {i - 1}")
        data = 0
        for k in range(size):
            data |= desc[i + k] << (8 * k)
        i += size
        items.append((btype, tag, data, size))
    return items


def hid_report_lengths(desc):
    """Per-report-ID (input, output, feature) bit totals from a HID descriptor.

    Walks the items tracking Report ID / Report Size / Report Count globals and
    accumulating each Main Input/Output/Feature item's bit count."""
    totals = {}
    report_id = 0
    report_size = 0
    report_count = 0
    depth = 0
    for btype, tag, data, _size in parse_hid_items(desc):
        if btype == 0 and tag == 0xA:  # Collection
            depth += 1
        elif btype == 0 and tag == 0xC:  # EndCollection
            depth -= 1
            if depth < 0:
                raise ValueError("HID EndCollection without a Collection")
        elif btype == 1 and tag == 0x8:  # Report ID
            report_id = data
        elif btype == 1 and tag == 0x7:  # Report Size
            report_size = data
        elif btype == 1 and tag == 0x9:  # Report Count
            report_count = data
        elif btype == 0 and tag in (0x8, 0x9, 0xB):  # Input / Output / Feature
            kind = {0x8: "input", 0x9: "output", 0xB: "feature"}[tag]
            slot = totals.setdefault(report_id, {"input": 0, "output": 0, "feature": 0})
            slot[kind] += report_size * report_count
    if depth != 0:
        raise ValueError(f"unbalanced HID collections (depth {depth})")
    return totals


def report_bytes(bits, with_id):
    """Bytes an item of `bits` bits occupies, plus the report ID byte when the
    interface uses numbered reports."""
    return (bits + 7) // 8 + (1 if with_id else 0)


# --- firmware image accessors ---------------------------------------------


def fw_symbols():
    return load_symbols(Path(VIAL_FIRMWARE).with_suffix(".map"))


@functools.lru_cache(maxsize=1)
def _cached_symbols():
    return fw_symbols()


@functools.lru_cache(maxsize=1)
def _cached_mem():
    return read_intel_hex(VIAL_FIRMWARE)


def fw_mem():
    return _cached_mem()


def code_bytes(name, length):
    syms = _cached_symbols()
    mem = _cached_mem()
    base = syms[name]
    return [mem[base + i] for i in range(length)]


def get_descriptor_iface(desc_type, iface=0, length=64):
    """GET_DESCRIPTOR targeted at an interface (bmRequestType 0x81)."""
    return [0x81, 0x06, 0x00, desc_type, iface & 0xFF, 0x00, length & 0xFF, (length >> 8) & 0xFF]


def generated_definition():
    """The xz blob the build baked from vial.json (parsed from the generated
    header next to the .hex)."""
    headers = sorted(Path(VIAL_FIRMWARE).parent.glob("*vial_definition.h"))
    if not headers:
        raise unittest.SkipTest("generated vial_definition.h not found next to the hex")
    text = headers[0].read_text()
    size = int(re.search(r"#define VIAL_DEFINITION_SIZE (\d+)", text).group(1))
    body = re.search(r"vial_definition\[\] = \{(.*?)\};", text, re.S)
    if not body:
        raise unittest.SkipTest("vial_definition[] not found in generated header")
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body.group(1)))
    if len(blob) != size:
        raise AssertionError(f"generated blob is {len(blob)} bytes, header says {size}")
    return blob


# --- interactive Vial session ---------------------------------------------


class VialSim(Rk61Sim):
    """An Rk61Sim booted on the vial image with helpers for the Vial protocol,
    the dynamic keymap, the layer engine and the tap/hold engine. Everything is
    driven cold (set_xram + call/break), so no real USB host is needed."""

    def __init__(self, firmware=None):
        super().__init__(firmware or VIAL_FIRMWARE)
        self.vial_in = self._static("usb", "vial_in")
        self.vial_out = self._static("usb", "vial_out")
        self.vial_pending = self._static("usb", "vial_request_pending")
        self.usb_state = self.sym["usb_device_state"]
        self.iface1_protocol = self.sym["interface1_protocol"]
        self.layer_state = self._static("matrix", "layer_state")
        self.default_layer = self.sym["default_layer"]
        self.tick_ms = self._static("tick", "tick_ms_counter")
        self.matrix_addr = self.sym["matrix"]
        self.matrix_prev = self.sym["matrix_previous"]
        self.matrix_updated = self.sym["matrix_updated"]
        self.report_addr = self.sym["keyboard_report"]

    # --- boot -------------------------------------------------------------
    def boot_usb(self, quiet=True):
        """Boot to the main loop, mark USB configured, and (by default) disable
        interrupts so the Timer2 scan ISR cannot overwrite the matrix[] cells a
        test injects. `quiet=False` keeps EA set for the EP0 transport test."""
        self.boot()
        self.set_xram(self.usb_state, [2])  # CONFIGURED
        if quiet:
            self.set_sfr(0xA8, 0x00)  # IEN0 = 0: no interrupt vectors

    def reboot(self):
        self.boot_usb()

    def reseed_store(self):
        """Erase both keymap sectors in the simulator's ROM, so the next boot
        finds neither valid and reseeds the defaults."""
        for addr in KEYMAP_SECTORS:
            self.cmd("set mem rom 0x%x 0xff" % addr)
        self.reboot()

    # --- protocol ---------------------------------------------------------
    def vial(self, req):
        """Run one 32-byte Vial request through vial_handle() and return the
        reply buffer. Bypasses the EP0 transport (tested separately)."""
        data = list(req)[:RAW_HID_REPORT_SIZE]
        data += [0] * (RAW_HID_REPORT_SIZE - len(data))
        self.set_xram(self.vial_in, data)
        self.set_xram(self.vial_pending, [1])
        self.call(self.sym["vial_task"])
        return self.get_xram(self.vial_out, RAW_HID_REPORT_SIZE)

    def set_keycode(self, layer, row, col, kc):
        self.vial([0x05, layer, row, col, (kc >> 8) & 0xFF, kc & 0xFF])

    def get_keycode(self, layer, row, col):
        out = self.vial([0x04, layer, row, col])
        return (out[4] << 8) | out[5]

    def reset_keycode(self, layer, row, col):
        self.vial([0x06, layer, row, col])

    def get_def(self):
        """Fetch the whole definition via CMD_VIAL_GET_SIZE + GET_DEFINITION
        blocks."""
        out = self.vial([0xFE, 0x01])
        size = out[0] | (out[1] << 8) | (out[2] << 16) | (out[3] << 24)
        blob = bytearray()
        for block in range((size + RAW_HID_REPORT_SIZE - 1) // RAW_HID_REPORT_SIZE):
            req = [0xFE, 0x02] + list(block.to_bytes(4, "little"))
            out = self.vial(req)
            blob += bytes(out[: min(RAW_HID_REPORT_SIZE, size - len(blob))])
        return bytes(blob)

    # --- EP0 -> EP2 transport --------------------------------------------
    def _fire_isr_to_sled(self):
        """Stage the NOP sled, point PC at it, and run until the interrupt (its
        flag already set) has vectored, handled, and returned to the sled end."""
        self.cmd("set mem rom 0x%x " % SLED + " ".join(["0x00"] * 16))
        self.cmd("pc 0x%x" % SLED)
        self.brk(SLED_END)
        self.run()
        self.cmd("delete")

    def vial_transport(self, req):
        """Drive a full SET_REPORT(Output) on interface 0: one SETUP plus four
        8-byte EP0 OUT packets, then vial_task() to answer on EP2 IN. Returns
        the EP2 IN reports seen (the SIE log caps each at 24 bytes)."""
        before = len(self.ep2_reports())
        self.set_xram(EP0_OUT_BUF, set_report_output(report_id=0, length=RAW_HID_REPORT_SIZE))
        self.set_sfr(USBIF1, SETUPIF)
        self._fire_isr_to_sled()

        data = list(req)[:RAW_HID_REPORT_SIZE]
        data += [0] * (RAW_HID_REPORT_SIZE - len(data))
        for i in range(4):
            self.set_xram(EP0_OUT_BUF, data[i * 8:(i + 1) * 8])
            self.set_sfr(USBIF2, OEP0IF)
            self._fire_isr_to_sled()

        self.call(self.sym["vial_task"])
        return self.ep2_reports()[before:]

    def unknown_feature_report(self):
        """SET_REPORT(Feature, id 0x99) on interface 1 -> the firmware STALLs."""
        setup = [0x21, 0x09, 0x99, 0x03, 0x01, 0x00, 0x05, 0x00]
        self.set_xram(EP0_OUT_BUF, setup)
        self.set_sfr(USBIF1, SETUPIF)
        self._fire_isr_to_sled()

    # --- keymap / layer state --------------------------------------------
    def layer_bits(self):
        b = self.get_xram(self.layer_state, 2)
        return b[0] | (b[1] << 8)

    def clear_layers(self):
        self.set_xram(self.layer_state, [0, 0])

    def default_layer_val(self):
        return self.get_xram(self.default_layer, 1)[0]

    def set_tick(self, ms):
        self.set_xram(self.tick_ms, [ms & 0xFF, (ms >> 8) & 0xFF, (ms >> 16) & 0xFF, (ms >> 24) & 0xFF])

    def report(self):
        return self.get_xram(self.report_addr, 8)

    def key_event(self, row, col, pressed, prev_pressed=None):
        """Inject one matrix transition directly into matrix[]/matrix_previous[]
        and run matrix_task(), so no timer/scan timing is involved."""
        if prev_pressed is None:
            prev_pressed = not pressed
        m = self.get_xram(self.matrix_addr, MATRIX_COLS)
        p = self.get_xram(self.matrix_prev, MATRIX_COLS)
        if pressed:
            m[col] |= 1 << row
        else:
            m[col] &= ~(1 << row)
        if prev_pressed:
            p[col] |= 1 << row
        else:
            p[col] &= ~(1 << row)
        self.set_xram(self.matrix_addr, m)
        self.set_xram(self.matrix_prev, p)
        self.set_xram(self.matrix_updated, [1])
        self.call(self.sym["matrix_task"])

    def tapping_task(self):
        self.call(self.sym["tapping_task"])

    # --- report capture ---------------------------------------------------
    def ep1_reports(self):
        return [[int(x, 16) for x in m.split()]
                for m in re.findall(r"\[SIE\] EP1 IN \d+ bytes:((?: [0-9a-f]{2})*)",
                                    self.stderr_text())]

    def ep2_reports(self):
        return [[int(x, 16) for x in m.split()]
                for m in re.findall(r"\[SIE\] EP2 IN \d+ bytes:((?: [0-9a-f]{2})*)",
                                    self.stderr_text())]


# --- descriptor tests ------------------------------------------------------


class TestVialDescriptors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # One simulator session for the whole class: each control transfer boots
        # from reset, so fetch everything the descriptor tests need up front.
        sim = Sim(firmware=VIAL_FIRMWARE)
        cls.config = sim.reassemble(sim.control_in(get_descriptor(DESC_CONFIGURATION)))
        hid_main = sim.reassemble(sim.control_in(get_descriptor_iface(USB_DESC_CLASS_HID, iface=0)))
        hid_extra = sim.reassemble(sim.control_in(get_descriptor_iface(USB_DESC_CLASS_HID, iface=1)))
        cls.report_main = code_bytes("hid_report_desc_vial_main", hid_main[7] | (hid_main[8] << 8))
        cls.report_extra = code_bytes("hid_report_desc_vial_extra", hid_extra[7] | (hid_extra[8] << 8))
        cls.serial = sim.reassemble(sim.control_in(get_descriptor(DESC_STRING, index=3)))

    @staticmethod
    def _interfaces(desc):
        out = []
        i = 0
        while i < len(desc):
            blen, btype = desc[i], desc[i + 1]
            if btype == 0x04:
                out.append(desc[i:i + blen])
            i += blen
        return out

    @staticmethod
    def _endpoints(desc):
        out = []
        i = 0
        while i < len(desc):
            blen, btype = desc[i], desc[i + 1]
            if btype == 0x05:
                out.append(desc[i:i + blen])
            i += blen
        return out

    def test_interface0_is_raw_hid(self):
        """Interface 0 is the unnumbered raw-HID Vial transport on EP2 IN."""
        iface0 = self._interfaces(self.config)[0]
        self.assertEqual(iface0[2], 0, "interface 0 number")
        self.assertEqual(iface0[5], 0x03, "interface 0 class HID")
        self.assertEqual(iface0[6], 0x00, "interface 0 subclass none")
        self.assertEqual(iface0[7], 0x00, "interface 0 protocol boot")
        self.assertEqual(iface0[4], 1, "interface 0 one endpoint")

    def test_interface1_is_boot_keyboard(self):
        """Interface 1 is the numbered boot keyboard (HID boot subclass, report
        protocol default) on EP1 IN."""
        ifaces = self._interfaces(self.config)
        endpoints = self._endpoints(self.config)
        iface1 = ifaces[1]
        self.assertEqual(iface1[2], 1, "interface 1 number")
        self.assertEqual(iface1[5], 0x03, "interface 1 class HID")
        self.assertEqual(iface1[6], 0x01, "interface 1 subclass boot")
        self.assertEqual(iface1[7], 0x01, "interface 1 protocol report")
        self.assertEqual(iface1[4], 1, "interface 1 one endpoint")

        self.assertEqual(len(endpoints), 2)
        ep0, ep1 = endpoints
        # Interface 0 endpoint: EP2 IN, 32-byte raw HID.
        self.assertEqual(ep0[2] & 0x0F, 2, "interface 0 endpoint 2")
        self.assertEqual(ep0[2] & 0x80, 0x80, "interface 0 endpoint IN")
        self.assertEqual(ep0[4] | (ep0[5] << 8), RAW_HID_REPORT_SIZE)
        # Interface 1 endpoint: EP1 IN, EP1_BUF_SIZE.
        self.assertEqual(ep1[2] & 0x0F, 1, "interface 1 endpoint 1")
        self.assertEqual(ep1[2] & 0x80, 0x80, "interface 1 endpoint IN")
        self.assertEqual(ep1[4] | (ep1[5] << 8), EP1_BUF_SIZE)

    def test_raw_hid_report_descriptor_parses(self):
        """Interface 0's report descriptor is a single balanced raw-HID
        collection (0xFF60/0x61) with no report ID."""
        desc = self.report_main
        items = parse_hid_items(desc)
        depth = 0
        report_ids = []
        usage_pages = []
        usages = []
        for btype, tag, data, _size in items:
            if btype == 0 and tag == 0xA:  # Collection
                depth += 1
            elif btype == 0 and tag == 0xC:
                depth -= 1
            elif btype == 1 and tag == 0x8:
                report_ids.append(data)
            elif btype == 1 and tag == 0x0:
                usage_pages.append(data)
            elif btype == 2 and tag == 0x0:
                usages.append(data)
        self.assertEqual(depth, 0, "raw HID collections must balance")
        self.assertEqual(report_ids, [], "raw HID interface must not use report IDs")
        self.assertIn(0xFF60, usage_pages, "raw HID usage page 0xFF60")
        self.assertIn(0x61, usages, "raw HID usage 0x61")

        lengths = hid_report_lengths(desc)
        self.assertEqual(report_bytes(lengths[0]["input"], with_id=False), RAW_HID_REPORT_SIZE)
        self.assertLessEqual(report_bytes(lengths[0]["input"], with_id=False), EP2_BUF_SIZE)

    def test_extra_report_descriptor_parses(self):
        """Interface 1's report descriptor balances, has report IDs 4/1/2 (and 5
        for ISP), and every input report fits the 16-byte EP1 buffer."""
        lengths = hid_report_lengths(self.report_extra)
        self.assertIn(REPORT_ID_KEYBOARD, lengths)
        self.assertIn(1, lengths, "system control report id 1")
        self.assertIn(2, lengths, "consumer control report id 2")
        self.assertIn(5, lengths, "ISP feature report id 5")
        for report_id, kinds in lengths.items():
            self.assertLessEqual(
                report_bytes(kinds["input"], with_id=True), EP1_BUF_SIZE,
                f"input report {report_id} does not fit EP1",
            )
            self.assertLessEqual(
                report_bytes(kinds["output"], with_id=True), EP1_BUF_SIZE,
                f"output report {report_id} does not fit EP1",
            )
        # The keyboard report is exactly the report id + the 8-byte 6KRO body.
        self.assertEqual(report_bytes(lengths[REPORT_ID_KEYBOARD]["input"], with_id=True), 9)

    def test_no_nkro_report_id(self):
        """Phase A ships 6KRO only: no report id 6 anywhere in interface 1."""
        lengths = hid_report_lengths(self.report_extra)
        self.assertNotIn(6, lengths, "no NKRO report id 6 before Phase E")

    def test_serial_string_contains_vial_magic(self):
        """The serial string must carry Vial's magic so the GUI lists the board."""
        desc = self.serial
        self.assertGreaterEqual(len(desc), 2)
        self.assertEqual(desc[1], DESC_STRING)
        text = bytes(desc[2:]).decode("utf-16-le", errors="replace")
        self.assertIn("vial:f64c2b3c", text)


# --- ISP recovery path -----------------------------------------------------


class TestVialIsp(unittest.TestCase):
    def test_isp_feature_report_jumps_on_vial(self):
        """sinowisp's feature report 5 on interface 1 must still reach the
        bootloader from the vial image (the anti-brick recovery path)."""
        sim = Sim(firmware=VIAL_FIRMWARE)
        out = sim.trigger_isp_jump()
        self.assertEqual(sim.stopped_at(out), sim.ISP_BOOTLOADER,
                         f"expected jump to 0x{sim.ISP_BOOTLOADER:x}; output:\n{out}")
        self.assertEqual(sim.acc(out), sim.ISP_MAGIC_ACC)
        self.assertEqual(sim.reg_b(out), sim.ISP_MAGIC_B)

    def test_isp_wrong_confirm_does_not_jump_on_vial(self):
        sim = Sim(firmware=VIAL_FIRMWARE)
        out = sim.trigger_isp_jump(confirm=(0x00, 0x00), run_task=False)
        self.assertEqual(sim.stopped_at(out), sim.SLED_END)
        self.assertNotEqual(sim.acc(out), sim.ISP_MAGIC_ACC)


# --- VIA / Vial protocol ---------------------------------------------------


class TestVialProtocol(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb()

    def tearDown(self):
        self.kb.close()

    def test_via_protocol_version(self):
        out = self.kb.vial([0x01])
        self.assertEqual((out[1] << 8) | out[2], 0x0009)

    def test_vial_keyboard_id(self):
        out = self.kb.vial([0xFE, 0x00])
        self.assertEqual(out[0], 6, "Vial protocol version 6")
        self.assertEqual(out[4:12], [0x52, 0x4B, 0x36, 0x31, 0x50, 0x6C, 0x75, 0x73],
                         "RK61Plus UID")

    def test_layer_count(self):
        self.assertEqual(self.kb.vial([0x11])[1], 4)

    def test_macro_count_and_buffer_size_zero(self):
        self.assertEqual(self.kb.vial([0x0C])[1], 0)
        out = self.kb.vial([0x0D])
        self.assertEqual((out[1], out[2]), (0, 0))

    def test_unlock_handshake_accepts_immediately(self):
        """VIAL_INSECURE: the unlock handshake completes with no key combo."""
        self.kb.vial([0xFE, 0x06])  # UNLOCK_START
        out = self.kb.vial([0xFE, 0x05])  # GET_UNLOCK_STATUS
        self.assertEqual(out[0], 1, "VIAL_INSECURE reports unlocked after start")
        self.assertEqual(self.kb.vial([0xFE, 0x07])[0], 1, "unlock poll reports unlocked")

    def test_get_def_matches_generated_blob(self):
        blob = self.kb.get_def()
        self.assertEqual(blob, generated_definition())

    def test_definition_json_shape(self):
        doc = __import__("json").loads(lzma.decompress(self.kb.get_def()))
        self.assertEqual(doc["matrix"]["rows"], 5)
        self.assertEqual(doc["matrix"]["cols"], 14)
        keys = [cell for row in doc["layouts"]["keymap"] for cell in row
                if isinstance(cell, str) and "," in cell]
        self.assertEqual(len(keys), 61, "61-key layout")
        self.assertEqual(len(doc["customKeycodes"]), 9, "9 custom board keys")

    def test_qmk_settings_query(self):
        out = self.kb.vial([0xFE, 0x09])
        qsids = [out[0] | (out[1] << 8), out[2] | (out[3] << 8), out[4] | (out[5] << 8)]
        self.assertEqual(qsids, [7, 22, 23])

    def test_qmk_settings_get_set(self):
        out = self.kb.vial([0xFE, 0x0A, 0x07, 0x00])
        self.assertEqual(out[4] | (out[5] << 8), 200, "default tapping term")
        self.kb.vial([0xFE, 0x0B, 0x07, 0x00, 0xFA, 0x00])  # 250
        out = self.kb.vial([0xFE, 0x0A, 0x07, 0x00])
        self.assertEqual(out[4] | (out[5] << 8), 250)
        self.kb.vial([0xFE, 0x0B, 0x16, 0x00, 0x00])  # permissive off
        self.assertEqual(self.kb.vial([0xFE, 0x0A, 0x16, 0x00])[4], 0)
        self.kb.vial([0xFE, 0x0C])  # reset
        self.assertEqual(self.kb.vial([0xFE, 0x0A, 0x07, 0x00])[4] | (
            self.kb.vial([0xFE, 0x0A, 0x07, 0x00])[5] << 8), 200)
        self.assertEqual(self.kb.vial([0xFE, 0x0A, 0x16, 0x00])[4], 1)

    def test_unhandled_via_command(self):
        self.assertEqual(self.kb.vial([0x7F])[0], 0xFF)


# --- EP0 SET_REPORT -> EP2 IN transport ------------------------------------


class TestVialTransport(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb(quiet=False)  # the EP0 ISR needs EA set

    def tearDown(self):
        self.kb.close()

    def test_set_report_roundtrip(self):
        """A Vial message written as four EP0 OUT packets gets a reply on EP2 IN
        within the same session."""
        reps = self.kb.vial_transport([0x01])
        self.assertTrue(reps, "vial_task should send an EP2 IN reply")
        self.assertEqual(reps[-1][:3], [0x01, 0x00, 0x09],
                         f"protocol version reply; got {reps[-1][:8]}")

    def test_unknown_feature_report_does_not_block_vial(self):
        """An unknown feature report STALLs, and the next Vial message still
        gets its reply."""
        self.kb.unknown_feature_report()
        reps = self.kb.vial_transport([0x01])
        self.assertTrue(reps, "Vial must still reply after an unknown feature report")
        self.assertEqual(reps[-1][:3], [0x01, 0x00, 0x09])


# --- dynamic keymap --------------------------------------------------------


class TestVialKeymap(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb()

    def tearDown(self):
        self.kb.close()

    def test_default_keycode(self):
        self.assertEqual(self.kb.get_keycode(0, ESC_ROW, ESC_COL), KC_ESC)
        self.assertEqual(self.kb.get_keycode(0, 0, 1), KC_1)

    def test_set_get_reset(self):
        self.kb.set_keycode(0, ESC_ROW, ESC_COL, KC_A)
        self.assertEqual(self.kb.get_keycode(0, ESC_ROW, ESC_COL), KC_A)
        self.kb.reset_keycode(0, ESC_ROW, ESC_COL)
        self.assertEqual(self.kb.get_keycode(0, ESC_ROW, ESC_COL), KC_ESC)

    def test_set_survives_reboot(self):
        self.kb.set_keycode(0, ESC_ROW, ESC_COL, KC_A)
        self.kb.reboot()
        self.assertEqual(self.kb.get_keycode(0, ESC_ROW, ESC_COL), KC_A)

    def test_empty_position_reads_no_and_ignores_write(self):
        # (3, 11) is an unmapped matrix position (no switch): slot 0xFF.
        self.assertEqual(self.kb.get_keycode(0, 3, 11), 0)
        self.kb.set_keycode(0, 3, 11, KC_A)
        self.assertEqual(self.kb.get_keycode(0, 3, 11), 0)

    def test_keymap_buffer_get_set(self):
        out = self.kb.vial([0x12, 0x00, 0x00, 0x04])
        self.assertEqual((out[4] << 8) | out[5], KC_ESC)
        self.assertEqual((out[6] << 8) | out[7], KC_1)
        self.kb.vial([0x13, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x05])  # A, B
        self.assertEqual(self.kb.get_keycode(0, ESC_ROW, ESC_COL), KC_A)
        self.assertEqual(self.kb.get_keycode(0, 0, 1), 0x0005)
        self.kb.reset_keycode(0, ESC_ROW, ESC_COL)
        self.kb.reset_keycode(0, 0, 1)

    def test_keypress_resolves_through_dynamic_map(self):
        """A key edited in the store must produce the edited keycode on EP1."""
        self.kb.set_keycode(0, ESC_ROW, ESC_COL, KC_A)
        self.kb.key_event(ESC_ROW, ESC_COL, True)
        reps = self.kb.ep1_reports()
        self.assertTrue(reps)
        self.assertEqual(reps[-1], [REPORT_ID_KEYBOARD, 0x00, 0x00, KC_A, 0, 0, 0, 0, 0])
        self.kb.key_event(ESC_ROW, ESC_COL, False)


# --- layer engine ----------------------------------------------------------


class TestVialLayers(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb()

    def tearDown(self):
        self.kb.close()

    def test_mo_and_transparent_fallthrough(self):
        """Fn is MO(1); Esc maps to GRV on layer 1. Releasing Fn must not clear
        the still-held key (no stuck/lost key across a layer change)."""
        self.kb.key_event(FN_ROW, FN_COL, True)
        self.assertEqual(self.kb.layer_bits(), 0x0002)
        self.kb.key_event(ESC_ROW, ESC_COL, True)
        self.assertEqual(self.kb.report()[2], KC_GRV)
        self.kb.key_event(FN_ROW, FN_COL, False)
        self.assertEqual(self.kb.layer_bits(), 0x0000)
        self.assertEqual(self.kb.report()[2], KC_GRV, "held key must survive the layer change")
        self.kb.key_event(ESC_ROW, ESC_COL, False)
        self.assertEqual(self.kb.report()[2], 0)

    def test_tg_toggles(self):
        self.kb.set_keycode(0, FN_ROW, 0, QK_TG | 1)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.layer_bits(), 0x0002)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.layer_bits(), 0x0000)

    def test_to_sets_layer(self):
        self.kb.set_keycode(0, FN_ROW, 0, QK_TO | 1)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.layer_bits(), 0x0002)

    def test_df_sets_default_without_persisting(self):
        self.kb.set_keycode(0, FN_ROW, 0, QK_DF | 2)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.default_layer_val(), 2)
        self.kb.reboot()
        self.assertEqual(self.kb.default_layer_val(), 0, "DF must not persist")

    def test_pdf_sets_default_and_persists(self):
        self.kb.set_keycode(0, FN_ROW, 0, QK_PDF | 2)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.default_layer_val(), 2)
        self.kb.reboot()
        self.assertEqual(self.kb.default_layer_val(), 2, "PDF must persist")

    def test_out_of_range_layer_clamped(self):
        self.kb.set_keycode(0, FN_ROW, 0, QK_TO | 0x1F)
        self.kb.key_event(FN_ROW, 0, True)
        self.kb.key_event(FN_ROW, 0, False)
        self.assertEqual(self.kb.layer_bits(), 1 << 3, "layer clamped to VIAL_LAYERS-1")


# --- tap / hold ------------------------------------------------------------


class TestVialTapHold(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb()
        self.kb.set_keycode(0, SPC_ROW, SPC_COL, QK_LAYER_TAP | (2 << 8) | KC_SPC)
        self.kb.set_keycode(2, RALT_ROW, RALT_COL, KC_LEFT)
        self.kb.set_keycode(0, 0, 1, KC_A)

    def tearDown(self):
        self.kb.close()

    def test_tap_under_term(self):
        self.kb.set_tick(0)
        self.kb.key_event(SPC_ROW, SPC_COL, True)
        before = len(self.kb.ep1_reports())
        self.kb.set_tick(10)
        self.kb.key_event(SPC_ROW, SPC_COL, False)
        reports = self.kb.ep1_reports()[before:]
        self.assertTrue(any(KC_SPC in r for r in reports), f"tap should send SPC; {reports}")
        self.assertEqual(self.kb.layer_bits(), 0, "a tap leaves no layer active")

    def test_hold_past_term(self):
        self.kb.set_tick(0)
        self.kb.key_event(SPC_ROW, SPC_COL, True)
        self.kb.set_tick(300)
        self.kb.tapping_task()
        self.assertEqual(self.kb.layer_bits(), 0x0004, "hold activates layer 2")
        self.kb.key_event(RALT_ROW, RALT_COL, True)
        self.assertEqual(self.kb.report()[2], KC_LEFT, "layer 2 RAlt is Left")
        self.kb.key_event(RALT_ROW, RALT_COL, False)
        self.kb.key_event(SPC_ROW, SPC_COL, False)
        self.assertEqual(self.kb.layer_bits(), 0)

    def test_permissive_hold_on_other_release(self):
        self.kb.set_tick(0)
        self.kb.key_event(SPC_ROW, SPC_COL, True)
        self.kb.set_tick(10)
        self.kb.key_event(0, 1, True)   # buffered
        self.kb.key_event(0, 1, False)  # permissive hold fires
        self.assertEqual(self.kb.layer_bits(), 0x0004)
        self.kb.key_event(SPC_ROW, SPC_COL, False)
        self.assertEqual(self.kb.layer_bits(), 0)

    def test_rolled_event_is_not_lost(self):
        """Press LT, press another key (buffered), release LT under term: the tap
        and the buffered press both reach the host."""
        self.kb.set_tick(0)
        self.kb.key_event(SPC_ROW, SPC_COL, True)
        self.kb.set_tick(10)
        self.kb.key_event(0, 1, True)
        self.kb.set_tick(20)
        self.kb.key_event(SPC_ROW, SPC_COL, False)
        reports = self.kb.ep1_reports()
        self.assertTrue(any(KC_SPC in r for r in reports), "tap keycode must be sent")
        self.assertTrue(any(KC_A in r for r in reports), "buffered press must be replayed")
        self.kb.key_event(0, 1, False)

    def test_tapping_term_change_affects_decision(self):
        """With a 10 ms term, a 50 ms press is a hold; with the default 200 ms it
        would be a tap."""
        self.kb.vial([0xFE, 0x0B, 0x07, 0x00, 0x0A, 0x00])  # term = 10
        self.kb.set_tick(0)
        self.kb.key_event(SPC_ROW, SPC_COL, True)
        self.kb.set_tick(50)
        self.kb.tapping_task()
        self.assertEqual(self.kb.layer_bits(), 0x0004)
        self.kb.key_event(SPC_ROW, SPC_COL, False)

    def test_settings_reset_restores_defaults(self):
        self.kb.vial([0xFE, 0x0B, 0x07, 0x00, 0xF4, 0x01])  # term = 500
        self.assertEqual(self.kb.vial([0xFE, 0x0A, 0x07, 0x00])[4] | (
            self.kb.vial([0xFE, 0x0A, 0x07, 0x00])[5] << 8), 500)
        self.kb.vial([0xFE, 0x0C])  # reset
        out = self.kb.vial([0xFE, 0x0A, 0x07, 0x00])
        self.assertEqual(out[4] | (out[5] << 8), 200)
        self.assertEqual(self.kb.vial([0xFE, 0x0A, 0x16, 0x00])[4], 1, "permissive hold restored")


# --- EP1 report protocol ---------------------------------------------------


# --- RF path stays 6KRO ----------------------------------------------------


class TestVialRfReport(unittest.TestCase):
    """The BLE/RF report path is unchanged by Vial: it still carries the plain
    6KRO frame, with no NKRO report id anywhere."""

    def setUp(self):
        self.kb = Rk61RfSim(firmware=VIAL_FIRMWARE)
        self.kb.boot()
        self.kb.set_band_24g(False)  # BLE
        self.kb.set_wired(False)     # wireless
        self.slave = RfSlave(self.kb)
        self.slave.install()

    def tearDown(self):
        self.slave.remove()
        self.kb.close()

    def test_rf_report_frame_is_6kro(self):
        # report_keyboard_t: mods, reserved, keys[6]; rf_send_report() drops the
        # reserved byte and sends mods + keys[0..4].
        ks = self.kb._a("keyboard_state")
        self.kb.set_xram(ks + 4, [1])  # connected
        self.kb.set_xram(self.kb._a("keyboard_report"),
                         [0x00, 0x00, KC_A, 0x05, 0x06, 0x00, 0x00, 0x00])
        self.kb.cold_call(self.kb._a("send_keyboard_report"), slave=self.slave)

        reports = [f for f in self.slave.frames if f[2] == RF_CMD_REPORT]
        self.assertTrue(reports, "expected an RF report frame")
        frame = reports[-1]
        self.assertEqual(len(frame), 32)
        self.assertEqual(frame[0:3], [RF_MAGIC, 0x1D, RF_CMD_REPORT])
        self.assertEqual(frame[3:9], [0x00, KC_A, 0x05, 0x06, 0x00, 0x00])
        self.assertEqual(frame[9], 0, "byte9 is 0 while a key is held")


class TestVialReportProtocol(unittest.TestCase):
    def setUp(self):
        self.kb = VialSim()
        self.kb.boot_usb()

    def tearDown(self):
        self.kb.close()

    def _press_and_capture(self):
        self.kb.set_keycode(0, ESC_ROW, ESC_COL, KC_A)
        self.kb.key_event(ESC_ROW, ESC_COL, True)
        reps = self.kb.ep1_reports()
        self.kb.key_event(ESC_ROW, ESC_COL, False)
        return reps[-1]

    def test_report_protocol_is_9_bytes_with_id(self):
        self.kb.set_xram(self.kb.iface1_protocol, [1])  # report protocol
        rpt = self._press_and_capture()
        self.assertEqual(len(rpt), 9, f"report protocol adds the report id; got {rpt}")
        self.assertEqual(rpt[0], REPORT_ID_KEYBOARD)
        self.assertEqual(rpt[3], KC_A)

    def test_boot_protocol_is_8_bytes(self):
        self.kb.set_xram(self.kb.iface1_protocol, [0])  # boot protocol
        rpt = self._press_and_capture()
        self.assertEqual(len(rpt), 8, f"boot protocol stays the plain 6KRO body; got {rpt}")
        self.assertEqual(rpt[2], KC_A)


if __name__ == "__main__":
    unittest.main()
