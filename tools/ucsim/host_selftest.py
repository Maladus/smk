#!/usr/bin/env python3
"""Self-test for the deferred Step 0 tooling (ucsim host mode + USB/IP bridge).

Runs the whole path on Linux without root or ``vhci-hcd``: it starts ucsim in
host mode, runs ``tools/usbip_bridge.py`` and drives the bridge with a minimal
fake-vhci USB/IP client. That exercises the same framing, control transfers and
pending interrupt-IN logic the kernel would use, so it catches bridge and
simulator regressions even where USB/IP attach is unavailable.

    tools/ucsim/host_selftest.py [firmware.hex]

Exit code 0 on success. With a Vial image it additionally round-trips a
``get_keyboard_id`` request over EP0 SET_REPORT -> EP2 IN.
"""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from usbip_bridge import (  # noqa: E402
    OP_REQ_DEVLIST, OP_REQ_IMPORT, USBIP_CMD_SUBMIT, USBIP_DIR_IN, USBIP_DIR_OUT,
    UcsimLink, USBIP_VERSION, query_device_descriptor,
)

UCSIM_PORT = 3271
BRIDGE_PORT = 3272
CONTROL_PORT = 3273


def recvn(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("socket closed")
        buf += chunk
    return buf


def wait_port(port: int, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"nothing listening on port {port}")


def start_ucsim(fw: str):
    ucsim = os.environ.get("SMK_UCSIM", os.path.join(ROOT, ".mise/ucsim/bin/ucsim-sh68f90"))
    env = dict(os.environ, SMK_UCSIM_HOST=str(UCSIM_PORT))
    proc_ucsim = subprocess.Popen(
        [ucsim, "-t", "sh68f90", "-e", f'file "{fw}"', "-g"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
    )
    wait_port(UCSIM_PORT)
    return proc_ucsim


def start_bridge():
    proc_bridge = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools/usbip_bridge.py"),
         "--ucsim-port", str(UCSIM_PORT), "--listen-port", str(BRIDGE_PORT),
         "--control-port", str(CONTROL_PORT),
         "--busid", "1-1", "-v"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    wait_port(BRIDGE_PORT)
    wait_port(CONTROL_PORT)
    return proc_bridge


def stop(*procs) -> None:
    for p in procs:
        p.terminate()
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()


def submit(sock: socket.socket, seq: int, ep: int, direction: int, setup: bytes,
           out_data: bytes = b"", length: int = 0, timeout: float = 5.0) -> tuple[int, bytes]:
    hdr = struct.pack(">IIIII", USBIP_CMD_SUBMIT, seq, 0, direction, ep)
    hdr += struct.pack(">Iiiii", 0, length, 0, 0, 0) + setup
    sock.sendall(hdr + out_data)
    sock.settimeout(timeout)
    h = recvn(sock, 48)
    _, _, _, _, _ = struct.unpack(">IIIII", h[:20])
    status, actual, _, _, _ = struct.unpack(">iiiii", h[20:40])
    data = recvn(sock, actual) if actual > 0 else b""
    return status, data


def direct_link_check(link: UcsimLink) -> None:
    dev = query_device_descriptor(link)
    assert dev[:2] == b"\x12\x01", dev
    assert link.control(bytes([0x00, 5, 1, 0, 0, 0, 0, 0])) == b""
    cfg = link.control(bytes([0x80, 6, 0, 2, 0, 0, 9, 0]), in_len=9)
    assert cfg and cfg[1] == 0x02, cfg
    total = struct.unpack_from("<H", cfg, 2)[0]
    full = link.control(bytes([0x80, 6, 0, 2, 0, 0, total & 0xFF, total >> 8]), in_len=total)
    assert full and len(full) == total, full
    assert link.control(bytes([0x00, 9, 1, 0, 0, 0, 0, 0])) == b""
    print(f"  link: device {dev[8] | dev[9] << 8:04x}:{dev[10] | dev[11] << 8:04x}, "
          f"config {len(full)} bytes OK")


def bridge_enum_check(sock: socket.socket) -> bytes:
    sock.sendall(struct.pack(">HHI", USBIP_VERSION, OP_REQ_DEVLIST, 0))
    common = recvn(sock, 8)
    (version, code, status) = struct.unpack(">HHI", common)
    assert code == 0x0005, hex(code)
    ndev = struct.unpack(">I", recvn(sock, 4))[0]
    devinfo = recvn(sock, 312 * ndev)

    sock.sendall(struct.pack(">HHI", USBIP_VERSION, OP_REQ_IMPORT, 0) + b"1-1".ljust(32, b"\0"))
    (version, code, status) = struct.unpack(">HHI", recvn(sock, 8))
    assert code == 0x0003, hex(code)
    recvn(sock, 312)

    st, dev = submit(sock, 1, 0, USBIP_DIR_IN, bytes([0x80, 6, 0, 1, 0, 0, 18, 0]), length=18)
    assert st == 0 and dev[:2] == b"\x12\x01", (st, dev)
    st, _ = submit(sock, 2, 0, USBIP_DIR_OUT, bytes([0x00, 5, 1, 0, 0, 0, 0, 0]))
    assert st == 0
    st, cfg = submit(sock, 3, 0, USBIP_DIR_IN, bytes([0x80, 6, 0, 2, 0, 0, 9, 0]), length=9)
    assert st == 0 and cfg[1] == 0x02, (st, cfg)
    total = struct.unpack_from("<H", cfg, 2)[0]
    st, full = submit(sock, 4, 0, USBIP_DIR_IN,
                      bytes([0x80, 6, 0, 2, 0, 0, total & 0xFF, total >> 8]), length=total)
    assert st == 0 and len(full) == total, (st, len(full))
    st, _ = submit(sock, 5, 0, USBIP_DIR_OUT, bytes([0x00, 9, 1, 0, 0, 0, 0, 0]))
    assert st == 0
    print(f"  bridge: DEVLIST ndev={ndev}, device {dev[8] | dev[9] << 8:04x}:{dev[10] | dev[11] << 8:04x}, "
          f"config {len(full)} bytes, SET_CONFIGURATION OK")
    return dev


def vial_roundtrip(sock: socket.socket) -> None:
    request = bytes([0xFE, 0x00]) + bytes(30)  # Vial get_keyboard_id
    setup = bytes([0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x20, 0x00])  # SET_REPORT(Output, iface 0)
    st, _ = submit(sock, 10, 0, USBIP_DIR_OUT, setup, out_data=request, length=32)
    assert st == 0, st
    # The reply is queued on EP2; the bridge keeps the interrupt-IN URB pending
    # until the firmware marks IEP2RDY.
    st, reply = submit(sock, 11, 2, USBIP_DIR_IN, bytes(8), length=32, timeout=10)
    assert st == 0 and len(reply) == 32, (st, len(reply))
    version = struct.unpack_from("<I", reply, 0)[0]
    assert version == 6, version
    print(f"  vial: get_keyboard_id -> protocol {version}, UID {reply[4:12].hex()} OK")


def matrix_typing(sock: socket.socket) -> None:
    """Type through the emulated RK61 Plus matrix: stage Esc (row 0, col 0) over
    the bridge's matrix-control channel while an interrupt-IN URB on EP1 is
    pending, and check the keyboard report carries Esc (0x29)."""
    control = socket.create_connection(("127.0.0.1", CONTROL_PORT), timeout=5)
    try:
        # Queue the EP1 interrupt IN first; it stays pending until the firmware
        # has a report (i.e. until the staged key is scanned).
        hdr = struct.pack(">IIIII", USBIP_CMD_SUBMIT, 30, 0, USBIP_DIR_IN, 1)
        hdr += struct.pack(">Iiiii", 0, 9, 0, 0, 0) + bytes(8)
        sock.sendall(hdr)
        control.sendall(b"press 0 0\n")
        sock.settimeout(10)
        h = recvn(sock, 48)
        status, actual, _, _, _ = struct.unpack(">iiiii", h[20:40])
        data = recvn(sock, actual) if actual > 0 else b""
        assert status == 0 and 0x29 in data, (status, data)
        control.sendall(b"release 0 0\n")
        print(f"  matrix: Esc -> report {data.hex()} OK")
    finally:
        control.close()


def main(argv: list[str]) -> int:
    fw = argv[0] if argv else os.path.join(ROOT, "build/royalkludge-rk61plus_vial_smk.hex")
    if not os.path.exists(fw):
        print(f"firmware not found: {fw}", file=sys.stderr)
        return 2
    proc_ucsim = start_ucsim(fw)
    proc_bridge = None
    try:
        link = UcsimLink("127.0.0.1", UCSIM_PORT)
        try:
            direct_link_check(link)
        finally:
            link.close()

        proc_bridge = start_bridge()
        sock = socket.create_connection(("127.0.0.1", BRIDGE_PORT), timeout=5)
        try:
            bridge_enum_check(sock)
            if "vial" in os.path.basename(fw):
                vial_roundtrip(sock)
            matrix_typing(sock)
        finally:
            sock.close()
        print("PASS")
        return 0
    finally:
        stop(*[p for p in (proc_bridge, proc_ucsim) if p])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
