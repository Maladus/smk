"""Test-side models of the *board* hardware wired to the SH68F90's pins.

The simulator (tools/ucsim/sh68f90.cc) models only the MCU: it exposes the GPIO
ports as plain SFR cells and knows nothing about what is soldered to them. The
board-level hardware -- here, the key matrix -- is emulated HERE, in the test,
by reacting to the firmware driving columns and reading rows.

Mechanism: uCsim is driven interactively over its command socket (`-Z <port>`),
which flushes immediately (a block-buffered stdout pipe would not) and, with
`-b`, is free of ANSI-color noise. The `-P` null prompt terminates every reply,
so each command round-trips in well under a millisecond. We set a breakpoint at
`user_matrix_read_rows`; on every hit we look at which column the firmware is
driving low and pull the matching rows low for the keys staged as pressed --
exactly what the physical matrix would do -- then continue. No matrix knowledge
lives in the simulator.
"""
import unittest
import re
import socket
import subprocess
import threading
import time

from sim import find_firmware, find_sim, load_symbols, read_intel_hex
from pathlib import Path

# --- Air60 matrix wiring (src/keyboards/nuphy-air60/kbdef.h) ----------------
# P-register SFR addresses (src/sino51lib/sh68f90/sh68f90.h).
P0, P1, P2, P3, P4, P5, P6, P7 = 0x80, 0x90, 0x98, 0xa0, 0xb0, 0x88, 0xc0, 0xf8

# Columns are driven low one at a time; each pressed key shorts its column to
# its row, so the row reads low while that column is driven.
COL_PIN = {0: (P5, 0), 1: (P5, 1), 2: (P5, 2),
           3: (P3, 5), 4: (P3, 4), 5: (P3, 3), 6: (P3, 2), 7: (P3, 1), 8: (P3, 0),
           9: (P2, 5), 10: (P2, 4), 11: (P2, 3), 12: (P2, 2), 13: (P2, 1), 14: (P2, 0),
           15: (P1, 5)}
ROW_PIN = {0: (P7, 1), 1: (P7, 2), 2: (P7, 3), 3: (P5, 3), 4: (P5, 4)}

# --- RK61 Plus matrix wiring (src/keyboards/royalkludge-rk61plus/kbdef.h) ---
# Columns C0-C7 = P6.0-7, C8-C10 = P5.0-2, C11 = P5.7, C12 = P4.0, C13 = P4.2;
# rows are the same as the Air60 (R0-R2 = P7.1-3, R3 = P5.3, R4 = P5.4).
RK61_COL_PIN = {0: (P6, 0), 1: (P6, 1), 2: (P6, 2), 3: (P6, 3), 4: (P6, 4), 5: (P6, 5), 6: (P6, 6), 7: (P6, 7),
                8: (P5, 0), 9: (P5, 1), 10: (P5, 2), 11: (P5, 7), 12: (P4, 0), 13: (P4, 2)}
RK61_ROW_PIN = {0: (P7, 1), 1: (P7, 2), 2: (P7, 3), 3: (P5, 3), 4: (P5, 4)}


class UcsimSession:
    """Interactive uCsim over its command socket, framed on the `-P` null prompt.

    The SIE model logs endpoint traffic to the simulator's stderr; a background
    thread drains it into `self.serr` so reports can be read back without ever
    blocking the command channel.
    """
    def __init__(self, firmware=None, sim=None, cpu="sh68f90"):
        self.sim = sim or find_sim()
        self.firmware = firmware or find_firmware()
        # A per-session port (not a fixed one), so independent sessions -- e.g. a
        # parallel test runner -- never collide on it. There is a tiny race
        # between picking a free port and uCsim binding it, so retry on a fresh
        # port if the launch never comes up.
        last_err = None
        for _ in range(5):
            self.port = self._free_port()
            self.proc = subprocess.Popen(
                [self.sim, "-b", "-P", "-Z", str(self.port), "-t", cpu],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE)
            self.serr = []
            self._serr_lock = threading.Lock()
            self._serr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
            self._serr_thread.start()

            self.sock = None
            end = time.time() + 5
            while self.sock is None and time.time() < end and self.proc.poll() is None:
                try:
                    self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.3)
                except OSError as e:
                    last_err = e
                    time.sleep(0.02)
            if self.sock is not None:
                break
            self.proc.kill()                 # this port did not come up; try another
            self._serr_thread.join(timeout=1)
        if self.sock is None:
            raise RuntimeError("uCsim command socket never opened (last: %r)" % last_err)
        # Without this every tiny command/reply pays ~40 ms of Nagle +
        # delayed-ACK latency -- crippling for the per-row-read reactive loop.
        # TCP_NODELAY disables our Nagle; uCsim sends its reply in several small
        # writes, so we also disable delayed-ACK (TCP_QUICKACK, re-armed after
        # every recv since it is one-shot) to keep its sends flowing. ~41 ms ->
        # ~0 ms per command.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._quickack = getattr(socket, "TCP_QUICKACK", None)
        self.sock.settimeout(10)
        self._drain_startup()
        self.cmd('file "%s"' % self.firmware)

    # --- plumbing ---------------------------------------------------------
    @staticmethod
    def _free_port():
        """An OS-assigned free TCP port to hand to uCsim's -Z (closed again right
        away, so it is free for uCsim to bind)."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def _drain_stderr(self):
        for line in iter(self.proc.stderr.readline, b""):
            with self._serr_lock:
                self.serr.append(line.decode("latin1"))

    def _drain_startup(self):
        """Consume the telnet handshake + banner (several NULs) until idle."""
        self.sock.settimeout(0.3)
        try:
            while True:
                if not self.sock.recv(4096):
                    break
        except OSError:
            pass
        self.sock.settimeout(10)

    def cmd(self, c):
        """Send one command; return its reply text up to the NUL prompt."""
        self.sock.sendall((c + "\n").encode())
        buf = bytearray()
        while b"\x00" not in buf:
            chunk = self.sock.recv(4096)
            if self._quickack is not None:
                self.sock.setsockopt(socket.IPPROTO_TCP, self._quickack, 1)
            if not chunk:
                break
            buf += chunk
        # strip the echoed command line and \r; drop everything past the NUL
        text = buf.split(b"\x00", 1)[0].decode("latin1").replace("\r", "")
        return text

    def stderr_text(self):
        with self._serr_lock:
            return "".join(self.serr)

    def close(self):
        try:
            self.cmd("quit")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
            try:
                self.proc.wait(timeout=2)
            except Exception:
                pass
        try:
            self.sock.close()
        except OSError:
            pass
        try:
            self.proc.stderr.close()
        except Exception:
            pass
        if self._serr_thread.is_alive():
            self._serr_thread.join(timeout=1)

    # --- typed accessors --------------------------------------------------
    def get_sfr(self, addr):
        # uCsim labels SFRs it knows with a verbose line and dumps the rest bare
        # (its 8051 names don't match the SH68F90 remapping -- e.g. 0x88 is really
        # P5, 0xf8 is P7 -- so only the value matters):
        #   0x88 TCON:            0b00000000 0x00 '.'   0     <- named
        #   0xf8                  00 .                        <- bare (P7)
        # Skip the echo, then read the named "0x<HH>" after the binary field,
        # falling back to the bare hex byte after the address.
        out = self.cmd("dump sfr 0x%02x 0x%02x" % (addr, addr))
        data = out.split("\n", 1)[1] if "\n" in out else out
        m = (re.search(r"0b[01]+\s+0x([0-9a-fA-F]{2})", data)
             or re.search(r"^\s*0x[0-9a-fA-F]+\s+([0-9a-fA-F]{2})\b", data, re.M))
        return int(m.group(1), 16) if m else None

    def set_sfr(self, addr, val):
        self.cmd("set mem sfr 0x%02x 0x%02x" % (addr, val))

    # Staging cells the SH68F90 model reads as the external pin level of P0 / P5 /
    # P7 (see sh68f90.cc). Writing here drives the pins the board would drive.
    PIN_STAGE = {P0: 0x1F16, P5: 0x1F15, P7: 0x1F17}

    def set_pin(self, port, level):
        """Set the external level the board presents on `port`'s pins (input bits
        read this; idle high == pull-ups). `port` is the SFR address (P0/P5/P7)."""
        self.cmd("set mem xram 0x%x 0x%02x" % (self.PIN_STAGE[port], level))

    def get_xram(self, addr, n=1):
        out = self.cmd("dump xram 0x%x 0x%x" % (addr, addr + n - 1))
        vals = []
        for line in out.splitlines():
            m = re.match(r"\s*0x[0-9a-fA-F]+\s+((?:[0-9a-fA-F]{2} )+)", line)
            if m:
                vals += [int(x, 16) for x in m.group(1).split()]
        return vals[:n]

    def brk(self, addr):
        self.cmd("break 0x%x" % addr)

    def run(self):
        return self.cmd("run")

    @staticmethod
    def stopped_at(out):
        m = re.findall(r"Stop at 0x([0-9a-fA-F]+)", out)
        return int(m[-1], 16) if m else None


class Air60Sim(UcsimSession):
    """A UcsimSession wired up as the Air60 board: the CONN_MODE switch and the
    key matrix (KeyMatrix) are driven test-side, so the simulator stays a pure
    SH68F90. Resolves the firmware addresses it needs from the SDCC `.map`."""

    def __init__(self, firmware=None, sim=None):
        super().__init__(firmware, sim)
        self.sym = load_symbols(Path(self.firmware).with_suffix(".map"))
        self.matrix = KeyMatrix()
        # Stack base = linker stack-start (__start__stack) minus 1 (SP reset),
        # matching the firmware's derived STACK_BASE so the high-water numbers
        # agree and don't drift as globals move SSEG. (__start__stack has no
        # area-letter prefix, so load_symbols doesn't see it.)
        self.stack_base = self._linker_sym("__start__stack") - 1

    def _a(self, name):
        # Firmware built before this symbol exists cannot exercise the path that
        # needs it; skip rather than fail so the harness can land ahead of it.
        if name not in self.sym:
            raise unittest.SkipTest(f"firmware has no {name} yet")
        return self.sym[name]

    def _linker_sym(self, name):
        pat = re.compile(r"^\s*([0-9A-Fa-f]+)\s+%s\b" % re.escape(name))
        with open(Path(self.firmware).with_suffix(".map")) as f:
            for line in f:
                m = pat.match(line)
                if m:
                    return int(m.group(1), 16)
        raise KeyError("%s not found in .map" % name)

    def mark_usb_configured(self):
        """Force usb_device_state = CONFIGURED (2). boot() stops at the main loop
        without driving real enumeration, so the firmware sits in DEFAULT; since
        usb_send_* gates on CONFIGURED, report-path tests must mark it first."""
        self.cmd("set mem xram 0x%x 0x02" % self._a("usb_device_state"))

    def boot(self, usb=True):
        """Boot from reset to the main loop. Only the real-time delays are
        skipped (they are calibrated busy-loops that would otherwise spin for
        millions of sim cycles). No matrix-buffer wipe is needed: the SH68F90
        model reads idle input pins high (pull-ups), so the boot-time scans see
        no keys. The CONN_MODE switch (P5.5) likewise idles high == USB; for RF
        we pull it low before kb_init samples it."""
        self.matrix.usb_mode = usb
        self.cmd("reset")
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_us"))   # RET
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_ms"))   # RET
        if not usb:
            self.set_pin(P5, 0xff & ~0x20)       # CONN_MODE = RF
        self.brk(self._a("kb_update_switches"))
        self.run()
        self.cmd("delete")

    def scan_for_report(self, max_hits=200):
        """Run the matrix scan, servicing every row read from the test-side
        KeyMatrix, until the firmware emits a new EP1 keyboard report (or
        max_hits row reads elapse). Returns all EP1 reports seen so far."""
        rr = self._a("user_matrix_read_rows")
        self.brk(rr)
        before = len(self.ep1_reports())
        for _ in range(max_hits):
            self.run()
            self.matrix.inject(self)
            if len(self.ep1_reports()) > before:
                break
        self.cmd("delete")
        return self.ep1_reports()

    def send_report_direct(self, keys):
        """Cold-invoke send_keyboard_report() with a staged 6KRO report and
        return the EP1 reports captured. This tests the report path itself,
        independent of the matrix; must be booted in USB mode first (so the
        report goes to EP1, not the RF link). Stages a return frame on the
        firmware's stack so the function returns onto a NOP sled at 0xF000."""
        report = [0x00, 0x00] + (list(keys) + [0] * 6)[:6]
        base = 0x85
        self.cmd("set mem rom 0xF000 " + " ".join(["0x00"] * 16))
        self.cmd("set mem xram 0x%x %s"
                 % (self._a("keyboard_report"), " ".join("0x%02x" % b for b in report)))
        self.cmd("set mem iram 0x%x 0x00" % (base + 1))
        self.cmd("set mem iram 0x%x 0xF0" % (base + 2))     # return high byte -> 0x90xx
        self.set_sfr(0x81, base + 2)                        # SP
        self.cmd("pc 0x%x" % self._a("send_keyboard_report"))
        self.brk(0xF000)
        self.run()
        self.cmd("delete")
        return self.ep1_reports()

    def ep1_reports(self):
        """Every EP1 (keyboard) report the SIE has logged, as byte lists."""
        return [[int(x, 16) for x in m.split()]
                for m in re.findall(r"\[SIE\] EP1 IN \d+ bytes:((?: [0-9a-f]{2})*)",
                                    self.stderr_text())]

    def _xdata_static(self, module, name):
        """Address of a module-static __xdata symbol (SDCC mangles statics as
        F<module>$<name>$..., which load_symbols' `_name` match skips)."""
        pat = re.compile(r"^[A-Z]:\s+([0-9A-Fa-f]+)\s+F%s\$%s\$"
                         % (re.escape(module), re.escape(name)))
        with open(Path(self.firmware).with_suffix(".map")) as f:
            for line in f:
                m = pat.match(line)
                if m:
                    return int(m.group(1), 16)
        raise KeyError("%s$%s not found in .map" % (module, name))

    def console_text(self):
        """Decode the console ring buffer to text. dprint_str() (the boot banner,
        key logs, ...) writes here; on hardware it is drained over USB EP2. Read
        at the main loop -- before console_task() first runs -- it still holds the
        boot banner."""
        cb = self._xdata_static("console", "console_buf")
        raw = self.get_xram(cb, 128)
        return "".join(chr(b) if 32 <= b < 127 else " " for b in raw)

    def fire_usb_vector(self):
        """Park on a NOP sled, raise USBIF1.SETUPIF, and let the CPU accept the
        USB interrupt. Returns (sp_before, sp_after, stopped_addr) so a test can
        check the 2-byte return-address push and the vector target (0x3b). Must
        be booted first (so EA / IEN1.EUSB are set by usb_init)."""
        self.cmd("set mem rom 0xF000 " + " ".join(["0x00"] * 16))   # NOP sled
        self.cmd("pc 0xF000")
        pre = self.get_sfr(0x81)                       # SP before the IRQ
        self.set_sfr(0x92, 0x10)                       # USBIF1.SETUPIF
        self.brk(0x3b)
        out = self.run()
        self.cmd("delete")
        post = self.get_sfr(0x81)                      # SP after the push
        return pre, post, self.stopped_at(out)

    def get_iram(self, addr, n):
        out = self.cmd("dump iram 0x%x 0x%x" % (addr, addr + n - 1))
        vals = []
        for line in out.splitlines():
            m = re.match(r"\s*0x[0-9a-fA-F]+\s+((?:[0-9a-fA-F]{2} )+)", line)
            if m:
                vals += [int(x, 16) for x in m.group(1).split()]
        return vals[:n]

    def nest_usb_descriptor(self):
        """Fire a GET_DESCRIPTOR(device) control transfer so the USB ISR (the
        deepest interrupt handler) runs nested over the main loop -- used to push
        the stack to its deepest reachable point. Leaves the EP0 IN packets in
        the SIE log."""
        setup = [0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00]  # GET_DESCRIPTOR device
        self.cmd("set mem xram 0x1100 " + " ".join("0x%02x" % b for b in setup))
        self.set_sfr(0x92, 0x10)                       # USBIF1.SETUPIF
        self.brk(self._a("kb_update_switches"))
        self.run()                                     # ISR fires during this run
        self.cmd("delete")

    # Top of the SH68F90's 256-byte internal RAM (stack base is per-build, see
    # self.stack_base, derived from __start__stack to match the firmware).
    STACK_TOP = 0xFF

    def paint_stack(self, sentinel=0xAA):
        """Fill the stack above the live frame with a sentinel, so
        stack_highwater() can tell afterwards how deep the firmware went.
        Painting starts above SP so the frames the run needs survive."""
        addr = self.get_sfr(0x81) + 1
        while addr <= self.STACK_TOP:
            # a long `set mem` echoes back more than one reply, so write a row at a time
            n = min(16, self.STACK_TOP - addr + 1)
            self.cmd("set mem iram 0x%02x " % addr + " ".join(["0x%02x" % sentinel] * n))
            addr += n

    def stack_highwater(self, base=None, sentinel=0xAA):
        """Bytes of the painted stack (base+1 .. 0xFF) used at the peak: the
        highest address still overwritten from its sentinel."""
        if base is None:
            base = self.stack_base
        vals = self.get_iram(base + 1, self.STACK_TOP - base)
        used = 0
        for i, b in enumerate(vals):
            if b != sentinel:
                used = i + 1
        return used


class KeyMatrix:
    """A keyboard's key matrix, emulated test-side. Holds the set of pressed
    (col, row) keys and, whenever the firmware is reading the rows, drives the
    external pin levels: each row idles high (pull-up) and is pulled low only if
    a pressed key on the currently-scanned column shorts it there. `col_pin` /
    `row_pin` map matrix positions to (SFR address, bit); they default to the
    Air60 wiring. The Air60 CONN_MODE switch (P5.5: 1=USB, 0=RF) is presented
    via `usb_mode`."""

    def __init__(self, col_pin=None, row_pin=None):
        self.pressed = set()
        self.usb_mode = True
        self.col_pin = COL_PIN if col_pin is None else col_pin
        self.row_pin = ROW_PIN if row_pin is None else row_pin

    def press(self, col, row):
        self.pressed.add((col, row))

    def release(self, col, row):
        self.pressed.discard((col, row))

    def clear(self):
        self.pressed.clear()

    def _col_driven_low(self, sess, col, port_cache):
        sfr, bit = self.col_pin[col]
        v = port_cache.get(sfr)
        if v is None:
            v = port_cache[sfr] = sess.get_sfr(sfr) or 0
        return (v & (1 << bit)) == 0

    def inject(self, sess):
        """Drive the external pin levels of P5/P7 for the column being scanned:
        idle rows high, pull a pressed key's row low only while its column is
        driven (so the multiplexed scan never sees a phantom)."""
        port_cache = {}
        low_rows = set()
        for (c, r) in self.pressed:
            if self._col_driven_low(sess, c, port_cache):
                low_rows.add(r)
        # P7 pin level: rows R0-R2 = bits 1-3 (rest float high)
        p7 = 0xFF
        for r in (0, 1, 2):
            if r in low_rows:
                p7 &= ~(1 << self.row_pin[r][1])
        sess.set_pin(P7, p7)
        # P5 pin level: rows R3-R4 = bits 3-4; bit5 = CONN_MODE (1=USB, 0=RF)
        p5 = 0xFF
        if not self.usb_mode:
            p5 &= ~0x20
        for r in (3, 4):
            if r in low_rows:
                p5 &= ~(1 << self.row_pin[r][1])
        sess.set_pin(P5, p5)


class Rk61Sim(UcsimSession):
    """A UcsimSession wired up as the RK61 Plus board. The simulator exposes the
    GPIO ports and the test drives the key matrix (KeyMatrix with RK61 maps); the
    boot/park port values are read straight back from the SFR cells.

    P0/P5/P7 are read direction-aware by the simulator: an input bit returns the
    staged external pin level (idle-high pull-ups), not the latch. P4 still reads
    its latch. The boot/park assertions therefore check each port's output bits
    only."""

    # P0 output bits checked at boot and after park. P0.4 (MOSI) is masked out:
    # the RF bring-up bit-bangs it and leaves it a released-high input, so it no
    # longer reads its boot latch. Boot keeps P0.2 (WAKE) + P0.5 (enable) high;
    # park leaves only P0.2 (WAKE) high among the checked bits.
    P0_OUT_BOOT = 0x24
    P0_OUT_PARK = 0xE7

    # P7CR at boot (P7.4 enable + P7.6 control are outputs) and after park
    # (P7.0 parked-only + P7.4 enable + P7.6/P7.7 control are outputs).
    P7_OUT_BOOT = 0x50
    P7_OUT_PARK = 0xD1

    def __init__(self, firmware=None, sim=None):
        super().__init__(firmware, sim)
        self.sym = load_symbols(Path(self.firmware).with_suffix(".map"))
        self.matrix = KeyMatrix(RK61_COL_PIN, RK61_ROW_PIN)

    def _a(self, name):
        # Firmware built before this symbol exists cannot exercise the path that
        # needs it; skip rather than fail so the harness can land ahead of it.
        if name not in self.sym:
            raise unittest.SkipTest(f"firmware has no {name} yet")
        return self.sym[name]

    def boot(self):
        """Boot from reset to the main loop (kb_update_switches). Only the
        calibrated delay loops are skipped; P0/P4/P7 are then at their stock boot
        values set by user_init()."""
        self.cmd("reset")
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_us"))   # RET
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_ms"))   # RET
        self.brk(self._a("kb_update_switches"))
        self.run()
        self.cmd("delete")

    def ports(self):
        """(P0, P4, P7) SFR values, as `dump sfr` reports them."""
        return self.get_sfr(P0), self.get_sfr(P4), self.get_sfr(P7)

    def park(self):
        """Cold-invoke user_sleep_prepare() (park_panel + INT4 wake arm) and
        return (P0, P4, P7) once it returns. A return frame is staged on the
        firmware's stack so the function RETs onto a NOP sled at 0xF000."""
        self.cmd("set mem rom 0xF000 " + " ".join(["0x00"] * 16))
        self.cmd("set mem iram 0x86 0x00")   # return low byte
        self.cmd("set mem iram 0x87 0xF0")   # return high byte -> 0xF000
        self.set_sfr(0x81, 0x87)             # SP
        self.cmd("pc 0x%x" % self._a("user_sleep_prepare"))
        self.brk(0xF000)
        self.run()
        self.cmd("delete")
        return self.ports()

    def set_xram(self, addr, data):
        """Write `data` (a byte list) to xdata starting at `addr`."""
        self.cmd("set mem xram 0x%x %s" % (addr, " ".join("0x%02x" % b for b in data)))

    def call(self, addr):
        """Cold-invoke the C function at `addr` and return once it RETs onto a
        NOP sled at 0xF000. The sled is re-staged each call, so a function can be
        invoked repeatedly against the same session."""
        self.cmd("set mem rom 0xF000 " + " ".join(["0x00"] * 16))
        self.cmd("set mem iram 0x86 0x00")   # return low byte
        self.cmd("set mem iram 0x87 0xF0")   # return high byte -> 0xF000
        self.set_sfr(0x81, 0x87)             # SP
        self.cmd("pc 0x%x" % addr)
        self.brk(0xF000)
        self.run()
        self.cmd("delete")

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

    def battery_loop_addr(self):
        """Address of the `JNB P0.0` count-loop head inside user_battery_measure()
        (opcode 0x30 0x80). The loop is the RC sense-flip detector; breaking here
        lets a test flip the staged P0.0 level after a scripted number of turns."""
        mem = read_intel_hex(self.firmware)
        base = self._a("user_battery_measure")
        for off in range(0x100):
            a = base + off
            if mem.get(a) == 0x30 and mem.get(a + 1) == 0x80:  # JNB P0.0, rel
                return a
        raise unittest.SkipTest("JNB P0.0 count loop not found in user_battery_measure")

    def battery_level_after(self, flip_after):
        """Invoke user_battery_measure() with P0.0 staged high, flip P0.0 low after
        `flip_after` count-loop turns, and return (battery_level, low_power) from
        keyboard_state. The delay loops are patched to RET and a return frame is
        staged so the function RETs onto the NOP sled at 0xF000."""
        self.cmd("reset")
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_us"))   # RET
        self.cmd("set mem rom 0x%x 0x22" % self._a("delay_ms"))   # RET
        self.set_pin(P0, 0xFF)                # sense pin P0.0 idles high
        self.cmd("set mem rom 0xF000 " + " ".join(["0x00"] * 16))
        self.cmd("set mem iram 0x86 0x00")
        self.cmd("set mem iram 0x87 0xF0")
        self.set_sfr(0x81, 0x87)              # SP
        self.cmd("pc 0x%x" % self._a("user_battery_measure"))

        loop = self.battery_loop_addr()
        self.brk(loop)
        for _ in range(flip_after + 1):       # reach the loop, then flip_after turns
            self.run()
        self.set_pin(P0, 0xFE)                # flip P0.0 low; next check exits
        self.cmd("delete")                    # drop the loop breakpoint

        self.brk(0xF000)
        self.run()                            # loop exits; RETs onto the sled
        self.cmd("delete")

        state = self._a("keyboard_state")
        return (self.get_xram(state + 2, 1)[0],   # battery_level
                self.get_xram(state + 3, 1)[0])   # low_power
