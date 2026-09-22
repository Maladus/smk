"""Test-side support for the RK61 Plus BK3632 RF link/pairing suite.

The RF tests need more than the board models in `devices.py`: an emulated SPI
slave and a handful of cold-call/XRAM helpers that drive firmware functions
directly. Those live here so `test_rk61_rf.py` is self-contained and can run
against a clean checkout. The generic helpers (`set_xram`, `call`, `_static`)
mirror the ones on `Rk61Sim`; they are repeated here rather than imported so the
RF suite does not depend on board-model changes that are still in flight.
"""

import re
import unittest
from pathlib import Path

from sim import read_intel_hex
from devices import Rk61Sim, P0, P4, P5


class Rk61RfSim(Rk61Sim):
    """`Rk61Sim` plus the cold-call / XRAM helpers the RF tests drive."""

    def set_xram(self, addr, data):
        """Write `data` (a byte list) to xdata starting at `addr`."""
        self.cmd("set mem xram 0x%x %s" % (addr, " ".join("0x%02x" % b for b in data)))

    def call(self, addr):
        """Cold-invoke the C function at `addr` and return once it RETs onto a
        NOP sled at 0x9000. The sled is re-staged each call, so a function can be
        invoked repeatedly against the same session."""
        self.cmd("set mem rom 0x9000 " + " ".join(["0x00"] * 16))
        self.cmd("set mem iram 0x86 0x00")   # return low byte
        self.cmd("set mem iram 0x87 0x90")   # return high byte -> 0x9000
        self.set_sfr(0x81, 0x87)             # SP
        self.cmd("pc 0x%x" % addr)
        self.brk(0x9000)
        self.run()
        self.cmd("delete")

    def mark_usb_configured(self):
        """Force usb_device_state = CONFIGURED (2). boot() stops at the main
        loop without driving real enumeration, so usb_is_configured() is false
        until this is called."""
        self.cmd("set mem xram 0x%x 0x02" % self._a("usb_device_state"))

    def set_band_24g(self, on):
        """Stage the P5.6 B/G band switch the firmware samples: G (low) =
        direct 2.4G, B (high) = BLE (pull-up idles high). Read-modify-write so it
        composes with set_wired()."""
        cur = self.get_xram(self.PIN_STAGE[P5], 1)[0]
        self.set_pin(P5, (cur & ~0x40) if on else (cur | 0x40))

    def set_wired(self, on):
        """Stage the P5.5 on/off switch: off (high) = wired/USB, on (low) =
        wireless. Read-modify-write so it composes with set_band_24g()."""
        cur = self.get_xram(self.PIN_STAGE[P5], 1)[0]
        self.set_pin(P5, (cur | 0x20) if on else (cur & ~0x20))

    def cold_call(self, addr, slave=None, dpl=None, dph=None, b=None, stack_arg=None):
        """Cold-invoke `addr` and return once it RETs onto the 0x9000 sled,
        servicing `slave`'s SPI breakpoints in between. The SDCC stack-auto frame
        puts a single stack argument at 0x85, below the return address at
        0x86/0x87; a first pointer/16-bit argument is passed in DPL/DPH, and a
        generic pointer's memory-space byte in B (0x00 = xdata)."""
        self.cmd("set mem rom 0x9000 " + " ".join(["0x00"] * 16))
        if stack_arg is not None:
            self.cmd("set mem iram 0x85 0x%02x" % (stack_arg & 0xFF))
        self.cmd("set mem iram 0x86 0x00")   # return low byte
        self.cmd("set mem iram 0x87 0x90")   # return high byte -> 0x9000
        self.set_sfr(0x81, 0x87)             # SP
        if dpl is not None:
            self.set_sfr(0x82, dpl & 0xFF)
        if dph is not None:
            self.set_sfr(0x83, dph & 0xFF)
        if b is not None:
            self.set_sfr(0xF0, b & 0xFF)
        self.cmd("pc 0x%x" % addr)
        if slave is None:
            self.brk(0x9000)
            self.run()
            self.cmd("delete")
            return

        # Keep the slave's breakpoints; add a numbered return breakpoint so only
        # it is removed when the call finishes.
        out = self.cmd("break 0x9000")
        m = re.search(r"Breakpoint (\d+)", out)
        nr = int(m.group(1)) if m else None
        while True:
            out = self.run()
            stop = self.stopped_at(out)
            if stop == 0x9000:
                break
            slave.service(stop)
        self.cmd("delete %d" % nr if nr is not None else "delete")

    def call_key(self, keycode, pressed, slave=None):
        """Cold-invoke kb_process_record(keycode, pressed): the 16-bit keycode in
        DPL/DPH, the bool pushed as the stack argument."""
        self.cold_call(self._a("kb_process_record"), slave=slave,
                       dpl=keycode & 0xFF, dph=(keycode >> 8) & 0xFF,
                       stack_arg=1 if pressed else 0)

    def _static(self, module, name):
        """Address of a module-static symbol. SDCC mangles file-scope statics as
        F<module>$<name> with an optional $<scope> suffix; load_symbols' leading-
        underscore match skips them, so resolve the map line directly."""
        pat = re.compile(r"^[A-Z]:\s+([0-9A-Fa-f]+)\s+F%s\$%s(?:\$[0-9_$]*)?\s"
                         % (re.escape(module), re.escape(name)))
        with open(Path(self.firmware).with_suffix(".map")) as f:
            for line in f:
                m = pat.match(line)
                if m:
                    return int(m.group(1), 16)
        raise KeyError("%s$%s not found in .map" % (module, name))


# --- BK3632 SPI slave (test-side) -------------------------------------------
# The firmware's rf_controller.c builds frames in rf_tx_buf and bit-bangs them
# out; the first byte of every outgoing frame is MAGIC_BYTE. rf_fetch_4() stages
# 0xff into rf_tx_buf before a read, which is how the slave tells a status fetch
# from an outgoing command.
RF_MAGIC = 0xAA
RF_CMD_LINK = 0x01
RF_CMD_REPORT = 0x02
RF_CMD_USB_MODE = 0x06


class RfSlave:
    """An emulated BK3632 SPI slave driven by two firmware breakpoints.

    The MCU bit-bangs SPI with no pollable chip-select handshake, so the slave
    is serviced at:
      * `bb_spi_burst` entry -- the start of one transfer. rf_tx_buf still holds
        the outgoing frame here, so it is snapshotted (report/link/pairing/USB
        commands) or, when the 0xff filler rf_fetch_4() staged is seen, the
        canned status reply is queued to shift out.
      * the `JNB P0.3` MISO sample in bb_spi_xfer_byte -- feed the next reply bit.

    It also toggles the P4.1 ACK on the first bit of a transfer so bb_spi_xfer()'s
    change-poll succeeds, and drives the P0.3 MISO input the firmware samples with
    `JNB`. The captured frames are what the tests assert on.
    """

    def __init__(self, kb):
        self.kb = kb
        self.rf_tx_buf = kb._a("rf_tx_buf")
        self.burst = kb._static("bb_spi", "bb_spi_burst")
        self.miso = self._miso_sample_addr()
        self.frames = []     # captured outgoing frames, as byte lists
        self.status = None   # 4-byte reply for the next status fetch
        self._reply = []
        self._bit = 0

    def _miso_sample_addr(self):
        """Address of the `JNB P0.3` instruction in bb_spi_xfer_byte, found by
        opcode so it survives rebuilds."""
        mem = read_intel_hex(self.kb.firmware)
        base = self.kb._a("bb_spi_xfer_byte")
        for off in range(0x100):
            if mem.get(base + off) == 0x30 and mem.get(base + off + 1) == 0x83:
                return base + off
        raise unittest.SkipTest("JNB P0.3 not found in bb_spi_xfer_byte")

    def install(self):
        self.kb.cmd("break 0x%x" % self.burst)
        self.kb.cmd("break 0x%x" % self.miso)

    def remove(self):
        self.kb.cmd("delete")

    def set_status(self, status0, status1):
        """Queue the reply rf_get_status() expects: 0xbb, checksum, s0, s1.
        status1 bit3 = connected, bit4 = paired, bits5-6 = rf_link."""
        self.status = [0xBB, (0x55 - status0 - status1) & 0xFF, status0, status1]

    def service(self, stop):
        if stop == self.burst:
            self._on_burst()
        elif stop == self.miso:
            self._on_miso()

    def _on_burst(self):
        raw = self.kb.get_xram(self.rf_tx_buf, 32)
        if raw and raw[0] == RF_MAGIC:
            self.frames.append(raw[:raw[1] + 3])
            self._reply = []
        else:
            # rf_fetch_4() staged 0xff; shift out the queued status reply.
            self._reply = list(self.status) if self.status else []
        self._bit = 0

    def _on_miso(self):
        if self._bit == 0:
            # Toggle the ACK so bb_spi_xfer() sees the change it polls for.
            p4 = self.kb.get_sfr(P4) or 0
            self.kb.set_sfr(P4, p4 ^ 0x02)
        if self._bit < len(self._reply) * 8:
            byte = self._reply[self._bit // 8]
            bit = (byte >> (7 - self._bit % 8)) & 1
        else:
            bit = 1  # idle high
        self._bit += 1
        self.kb.set_pin(P0, 0xFF if bit else 0xF7)
