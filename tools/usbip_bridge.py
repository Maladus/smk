#!/usr/bin/env python3
"""USB/IP bridge for the ucsim SH68F90 host mode (Linux, standard library only).

Runs the firmware in ucsim (``SMK_UCSIM_HOST=<port>``, see
``tools/ucsim/start-usbip-device.sh``) and serves the USB/IP protocol on
TCP/3240 so the kernel's ``vhci-hcd`` driver can attach the simulated device.
``usbip attach`` then makes hidraw, the HID stack and the Vial GUI talk to the
simulated firmware exactly like to real hardware.

The bridge turns each USB/IP URB into SIE transactions on ucsim's host-mode
socket. Control transfers are synchronous; interrupt IN URBs stay pending until
the firmware marks the endpoint ready (``IEPnRDY``), which mirrors the real
device NAKing until it has data.
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
import time

# --- ucsim host-mode framing -------------------------------------------------
# [u16 be length][u8 opcode][payload]; replies [u16 be length][u8 status][data]
H_RESET = 0x01
H_SETUP = 0x02
H_OUT = 0x03
H_IN = 0x04
H_SOF = 0x05
H_QUIT = 0x06
H_WAIT = 0x07
H_KEY = 0x0A

# Reply status bytes
ST_OK = 0x4B  # 'K'
ST_DATA = 0x44  # 'D'
ST_NAK = 0x4E  # 'N'
ST_STALL = 0x54  # 'T'

EP0_MAXPACKET = 8

# --- USB/IP protocol ---------------------------------------------------------
USBIP_VERSION = 0x0111

OP_REQ_DEVLIST = 0x8005
OP_REP_DEVLIST = 0x0005
OP_REQ_IMPORT = 0x8003
OP_REP_IMPORT = 0x0003

USBIP_CMD_SUBMIT = 0x0001
USBIP_RET_SUBMIT = 0x0003
USBIP_CMD_UNLINK = 0x0002
USBIP_RET_UNLINK = 0x0004

USBIP_DIR_OUT = 0
USBIP_DIR_IN = 1

USB_SPEED_FULL = 2

USBIP_HEADER_LEN = 48  # 20 basic + 28 cmd_submit
USBIP_RET_SUBMIT_LEN = 20

EPIPE = -32
ETIMEDOUT = -110


class UcsimError(RuntimeError):
    pass


class UcsimLink:
    """Client for ucsim's host-mode TCP socket."""

    def __init__(self, host: str, port: int, timeout: float = 2.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def close(self) -> None:
        try:
            self.send(H_QUIT)
        except OSError:
            pass
        self.sock.close()

    def _recvn(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise UcsimError("ucsim host socket closed")
            buf += chunk
        return buf

    def send(self, op: int, payload: bytes = b"") -> None:
        body = bytes([op]) + payload
        self.sock.sendall(struct.pack(">H", len(body)) + body)

    def recv(self) -> bytes:
        (ln,) = struct.unpack(">H", self._recvn(2))
        return self._recvn(ln)

    # -- transactions --------------------------------------------------------
    def reset(self) -> None:
        self.send(H_RESET)
        self.recv()

    def sof(self) -> None:
        self.send(H_SOF)
        self.recv()

    def set_key(self, row: int, col: int, pressed: bool) -> None:
        """Stage a matrix key in the simulator (host-mode typing)."""
        self.send(H_KEY, bytes([row, col, 1 if pressed else 0]))
        self.recv()

    def wait_idle(self, timeout: float = 2.0) -> None:
        """Block until the firmware has finished the previous control transfer:
        no EP0 completion flag pending and no OUT data still queued. This is the
        handshake that stops the host from starting the next SETUP while the
        (much slower) simulated firmware is still processing the last one."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.send(H_WAIT)
            r = self.recv()
            if r and r[0] == ST_OK:
                return
            time.sleep(0.001)
        raise UcsimError("control endpoint did not go idle")

    def setup(self, setup_bytes: bytes) -> None:
        assert len(setup_bytes) == 8
        self.send(H_SETUP, setup_bytes)
        self.recv()

    def out(self, data: bytes) -> None:
        assert len(data) <= EP0_MAXPACKET
        self.send(H_OUT, bytes([len(data)]) + data)
        self.recv()

    def poll_in(self, ep: int) -> tuple[int, bytes]:
        """Return (status_byte, payload). status is ST_DATA/ST_NAK/ST_STALL."""
        self.send(H_IN, bytes([ep]))
        r = self.recv()
        if not r:
            raise UcsimError("empty IN reply")
        return r[0], r[1:]

    # -- control transfers ---------------------------------------------------
    def _settle(self) -> None:
        self.wait_idle()

    def control(self, setup_bytes: bytes, out_data: bytes = b"", in_len: int = 0,
                timeout: float = 2.0) -> bytes | None:
        """Run one control transfer. Returns the IN payload, or None on stall.

        The status stage direction is opposite to the data stage: control IN
        ends with an OUT zero-length packet, everything else with an IN."""
        self.setup(setup_bytes)
        is_in = bool(setup_bytes[0] & 0x80)
        if is_in:
            data = b""
            deadline = time.monotonic() + timeout
            last_full = False
            while len(data) < in_len and time.monotonic() < deadline:
                st, pkt = self.poll_in(0)
                if st == ST_DATA:
                    data += pkt
                    last_full = len(pkt) == EP0_MAXPACKET
                    if len(pkt) < EP0_MAXPACKET:
                        break
                elif st == ST_NAK:
                    time.sleep(0.001)
                elif st == ST_STALL:
                    return None
                else:
                    raise UcsimError(f"unexpected IN status 0x{st:02x}")
            # A full final packet is followed by a zero-length packet that ends
            # the data stage; consume it if present.
            if last_full and len(data) >= in_len:
                self.poll_in(0)
            self.out(b"")  # OUT status
            self._settle()
            return data
        # OUT direction
        for off in range(0, len(out_data), EP0_MAXPACKET):
            self.out(out_data[off:off + EP0_MAXPACKET])
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            st, _ = self.poll_in(0)
            if st == ST_DATA:
                self._settle()
                return b""
            if st == ST_NAK:
                time.sleep(0.001)
                continue
            if st == ST_STALL:
                return None
            raise UcsimError(f"unexpected IN status 0x{st:02x}")
        raise UcsimError("control OUT status timed out")


class UsbIpDevice:
    """Device identity advertised in DEVLIST/IMPORT replies."""

    def __init__(self, descriptor: bytes, busid: str, busnum: int, devnum: int):
        self.path = b"\x00" * 256
        self.busid = busid.encode()[:31].ljust(32, b"\x00")
        self.busnum = busnum
        self.devnum = devnum
        self.speed = USB_SPEED_FULL
        self.id_vendor = struct.unpack_from("<H", descriptor, 8)[0]
        self.id_product = struct.unpack_from("<H", descriptor, 10)[0]
        self.bcd_device = struct.unpack_from("<H", descriptor, 12)[0]
        self.b_device_class = descriptor[4]
        self.b_device_subclass = descriptor[5]
        self.b_device_protocol = descriptor[6]
        self.b_configuration_value = 1
        self.b_num_configurations = descriptor[17] if len(descriptor) > 17 else 1
        self.b_num_interfaces = 0

    def pack(self) -> bytes:
        return (
            self.path
            + self.busid
            + struct.pack(
                ">IIIHHHBBBBBB",
                self.busnum,
                self.devnum,
                self.speed,
                self.id_vendor,
                self.id_product,
                self.bcd_device,
                self.b_device_class,
                self.b_device_subclass,
                self.b_device_protocol,
                self.b_configuration_value,
                self.b_num_configurations,
                self.b_num_interfaces,
            )
        )


class PendingUrb:
    __slots__ = ("seqnum", "devid", "ep", "direction", "length", "retry_at", "deadline")

    def __init__(self, seqnum: int, devid: int, ep: int, direction: int, length: int):
        self.seqnum = seqnum
        self.devid = devid
        self.ep = ep
        self.direction = direction
        self.length = length
        self.retry_at = 0.0
        self.deadline = time.monotonic() + 5.0


class UsbIpBridge:
    def __init__(self, link: UcsimLink, device: UsbIpDevice, listen_port: int,
                 host: str = "127.0.0.1", verbose: bool = False, control_port: int = 0):
        self.link = link
        self.device = device
        self.host = host
        self.listen_port = listen_port
        self.verbose = verbose
        self.pending: list[PendingUrb] = []
        self.sof_at = 0.0
        self.in_control = False
        # Matrix control channel: a local TCP port that accepts `press r c` /
        # `release r c` / `clear` lines and forwards them to the simulator, so a
        # test can type through the emulated key matrix.
        self.control_port = control_port
        self.control_srv: socket.socket | None = None
        self.control_conn: socket.socket | None = None
        self.control_buf = b""

    def _service_control(self) -> None:
        if self.control_srv is None:
            return
        if self.control_conn is None:
            try:
                conn, _ = self.control_srv.accept()
                conn.setblocking(False)
                self.control_conn = conn
                self.control_buf = b""
            except BlockingIOError:
                return
        try:
            data = self.control_conn.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            self.control_conn.close()
            self.control_conn = None
            return
        if not data:
            self.control_conn.close()
            self.control_conn = None
            return
        self.control_buf += data
        while b"\n" in self.control_buf:
            line, self.control_buf = self.control_buf.split(b"\n", 1)
            self._handle_control_line(line.decode("ascii", "replace").strip())

    def _handle_control_line(self, line: str) -> None:
        parts = line.split()
        self.log("control:", line)
        try:
            if parts and parts[0] == "press":
                self.link.set_key(int(parts[1]), int(parts[2]), True)
            elif parts and parts[0] == "release":
                self.link.set_key(int(parts[1]), int(parts[2]), False)
            elif parts and parts[0] == "clear":
                for r in range(8):
                    for c in range(16):
                        self.link.set_key(r, c, False)
        except (IndexError, ValueError):
            pass

    def log(self, *args) -> None:
        if self.verbose:
            print("[bridge]", *args, file=sys.stderr)

    # -- USB/IP framing ------------------------------------------------------
    @staticmethod
    def _recvn(sock: socket.socket, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("client closed")
            buf += chunk
        return buf

    def _send_ret_submit(self, sock: socket.socket, urb: PendingUrb,
                         status: int, data: bytes) -> None:
        hdr = struct.pack(">IIIII", USBIP_RET_SUBMIT, urb.seqnum, urb.devid,
                          urb.direction, urb.ep)
        hdr += struct.pack(">iiiii", status, len(data), 0, 0, 0)
        hdr += b"\x00" * 8  # header is always 48 bytes (union sized by cmd_submit)
        sock.sendall(hdr + data)

    def _send_ret_unlink(self, sock: socket.socket, seqnum: int, status: int = 0) -> None:
        hdr = struct.pack(">IIIII", USBIP_RET_UNLINK, seqnum, 0, 0, 0)
        hdr += struct.pack(">i", status)
        hdr += b"\x00" * 24  # pad the union to the fixed 48-byte header
        sock.sendall(hdr)

    def _handle_import(self, sock: socket.socket, code: int) -> None:
        if code == OP_REQ_DEVLIST:
            common = struct.pack(">HHI", USBIP_VERSION, OP_REP_DEVLIST, 0)
            sock.sendall(common + struct.pack(">I", 1) + self.device.pack())
        elif code == OP_REQ_IMPORT:
            common = struct.pack(">HHI", USBIP_VERSION, OP_REP_IMPORT, 0)
            sock.sendall(common + self.device.pack())
            # The kernel's vhci handles addressing itself and never forwards
            # SET_ADDRESS, but the SMK firmware only accepts SET_CONFIGURATION
            # once it has been addressed. Supply it so the device reaches
            # CONFIGURED and actually sends reports.
            try:
                self.link.control(bytes([0x00, 5, 1, 0, 0, 0, 0, 0]))
            except UcsimError as exc:
                self.log("SET_ADDRESS inject failed:", exc)
        else:
            raise ValueError(f"unexpected op {code:#x}")

    # -- URB handling --------------------------------------------------------
    def _handle_urb(self, sock: socket.socket, urb: PendingUrb, payload: bytes) -> None:
        if urb.ep == 0:
            self.in_control = True
            try:
                result = self._control(urb, payload)
            finally:
                self.in_control = False
            if result is None:
                self._send_ret_submit(sock, urb, EPIPE, b"")
            else:
                self._send_ret_submit(sock, urb, 0, result)
            return
        # Interrupt IN: complete now or keep pending until the endpoint is ready.
        self.pending.append(urb)
        self._try_pending(sock)

    def _control(self, urb: PendingUrb, payload: bytes) -> bytes | None:
        setup = payload[:8]
        out_data = payload[8:8 + urb.length] if urb.direction == USBIP_DIR_OUT else b""
        in_len = urb.length if urb.direction == USBIP_DIR_IN else 0
        return self.link.control(setup, out_data, in_len)

    def _try_pending(self, sock: socket.socket) -> None:
        now = time.monotonic()
        for urb in list(self.pending):
            if now < urb.retry_at:
                continue
            st, pkt = self.link.poll_in(urb.ep)
            if st == ST_DATA:
                self.pending.remove(urb)
                self.log("complete ep", urb.ep, "len", len(pkt), "data", pkt.hex(), "urb_len", urb.length)
                data = pkt[:urb.length] if urb.length else pkt
                self._send_ret_submit(sock, urb, 0, data)
            elif st == ST_STALL:
                self.pending.remove(urb)
                self._send_ret_submit(sock, urb, EPIPE, b"")
            else:  # NAK: retry shortly
                urb.retry_at = now + 0.001
                if now > urb.deadline:
                    self.pending.remove(urb)
                    self._send_ret_submit(sock, urb, ETIMEDOUT, b"")

    def _maybe_sof(self) -> None:
        now = time.monotonic()
        # Only skip SOFs during a control transfer. A permanently pending
        # interrupt-IN URB (the kernel's usbhid keeps one queued) must not stop
        # the frame ticks, or the firmware loses its bus timing.
        if self.in_control:
            return
        if now - self.sof_at < 0.001:
            return
        self.sof_at = now
        self.link.sof()

    # -- server --------------------------------------------------------------
    def serve_forever(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.listen_port))
        srv.listen(1)
        print(f"[bridge] USB/IP listening on {self.host}:{self.listen_port}", file=sys.stderr)
        if self.control_port:
            self.control_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.control_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.control_srv.bind((self.host, self.control_port))
            self.control_srv.listen(1)
            self.control_srv.setblocking(False)
            print(f"[bridge] matrix control on {self.host}:{self.control_port}", file=sys.stderr)
        try:
            while True:
                conn, addr = srv.accept()
                self.log("kernel connected from", addr)
                try:
                    self._serve_client(conn)
                except (EOFError, OSError) as exc:
                    self.log("kernel disconnected:", exc)
                finally:
                    conn.close()
        finally:
            srv.close()
            if self.control_srv is not None:
                self.control_srv.close()

    def _serve_client(self, sock: socket.socket) -> None:
        sock.settimeout(0.001)
        buf = b""
        imported = False
        while True:
            # 0. Forward matrix key presses from the control channel.
            self._service_control()
            # 1. Drain USB/IP bytes from the kernel.
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    raise EOFError("client closed")
                buf += chunk
            except socket.timeout:
                pass
            while True:
                if len(buf) < 8:
                    break
                version, code, status = struct.unpack(">HHI", buf[:8])
                if code in (OP_REQ_DEVLIST, OP_REQ_IMPORT):
                    need = 8 if code == OP_REQ_DEVLIST else 8 + 32
                    if len(buf) < need:
                        break
                    buf = buf[need:]
                    self._handle_import(sock, code)
                    imported = True
                    self.log("handled import op", hex(code))
                elif code == USBIP_CMD_SUBMIT:
                    if len(buf) < USBIP_HEADER_LEN:
                        break
                    (seqnum, devid, direction, ep) = struct.unpack(">IIII", buf[4:20])
                    (tflags, tlen, sframe, npkts, interval) = struct.unpack(">Iiiii", buf[20:40])
                    setup = buf[40:48]
                    need = USBIP_HEADER_LEN + (tlen if direction == USBIP_DIR_OUT and tlen > 0 else 0)
                    if len(buf) < need:
                        break
                    payload = buf[40:need]  # setup + OUT data
                    buf = buf[need:]
                    urb = PendingUrb(seqnum, devid, ep, direction, max(tlen, 0))
                    self._handle_urb(sock, urb, payload)
                elif code == USBIP_CMD_UNLINK:
                    if len(buf) < 8 + 4:
                        break
                    (target,) = struct.unpack(">I", buf[8:12])
                    buf = buf[12:]
                    for urb in list(self.pending):
                        if urb.seqnum == target:
                            self.pending.remove(urb)
                    self._send_ret_unlink(sock, target)
                else:
                    raise ValueError(f"unknown USB/IP code {code:#x}")
            # 2. Retry pending interrupt URBs.
            if self.pending:
                self._try_pending(sock)
            # 3. Keep the simulated bus alive with SOFs.
            self._maybe_sof()


def query_device_descriptor(link: UcsimLink, attempts: int = 5) -> bytes:
    """Read the device descriptor, tolerating a device still settling after a
    bus reset (the firmware re-runs usb_init() in its reset ISR)."""
    setup = bytes([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00])
    last = None
    for _ in range(attempts):
        link.reset()
        time.sleep(0.05)
        desc = link.control(setup, in_len=18, timeout=1.0)
        if desc is not None and len(desc) >= 18 and desc[1] == 0x01:
            return desc
        last = desc
        time.sleep(0.05)
    raise UcsimError(f"bad device descriptor: {last!r}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ucsim-host", default="127.0.0.1")
    ap.add_argument("--ucsim-port", type=int, default=int(os.environ.get("SMK_UCSIM_HOST_PORT", "3241")))
    ap.add_argument("--listen-host", default="127.0.0.1")
    ap.add_argument("--listen-port", type=int, default=3240)
    ap.add_argument("--busid", default=os.environ.get("SMK_USBIP_BUSID", "1-1"))
    ap.add_argument("--busnum", type=int, default=1)
    ap.add_argument("--devnum", type=int, default=1)
    ap.add_argument("--control-port", type=int, default=0,
                    help="local TCP port for matrix key injection (0 = disabled)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    link = UcsimLink(args.ucsim_host, args.ucsim_port)
    try:
        descriptor = query_device_descriptor(link)
        device = UsbIpDevice(descriptor, args.busid, args.busnum, args.devnum)
        print(f"[bridge] device {device.id_vendor:04x}:{device.id_product:04x} "
              f"busid {args.busid}", file=sys.stderr)
        UsbIpBridge(link, device, args.listen_port, args.listen_host, args.verbose,
                    args.control_port).serve_forever()
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
