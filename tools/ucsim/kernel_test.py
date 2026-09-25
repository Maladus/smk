#!/usr/bin/env python3
"""End-to-end test: attach the simulated RK61 Plus to the real Linux kernel.

Runs the firmware in ucsim host mode, bridges it to the kernel over USB/IP, and
checks the things the simulator alone cannot: real enumeration, a key typed
through the emulated matrix arriving as an input event, and a Vial request
round-tripping through the kernel's raw-HID interface.

Linux only. Needs `vhci-hcd`, an `usbip` binary, and a way to get root for the
root-only vhci sysfs write. Root is obtained from a root container sharing this
kernel (no `--privileged` needed), so Docker is required unless you are already
root.

    tools/ucsim/kernel_test.py [firmware.hex]

Exit code 0 on success, 1 on failure, 2 on a missing prerequisite.
"""

from __future__ import annotations

import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

UCSIM_PORT = 3271
BRIDGE_PORT = 3240
CONTROL_PORT = 3273
USBIP_BIN = os.environ.get("SMK_USBIP_BIN", "/run/usbipd-win/usbip")
IMAGE = os.environ.get("SMK_USBIP_IMAGE", "alpine:3.20")
VENDOR_PRODUCT = "258a:00f8"
FIFO = "/tmp/smk_kernel_test.fifo"


def run(cmd, binary: bool = False, timeout: float = 20.0):
    try:
        return subprocess.run(cmd, capture_output=True, text=not binary, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or (b"" if binary else "")
        err = exc.stderr or (b"" if binary else "")
        return subprocess.CompletedProcess(cmd, 124, out, err)


def docker(*args, devices=(), binary: bool = False, timeout: float = 20.0):
    cmd = ["docker", "run", "--rm"]
    for d in devices:
        cmd += ["--device", d]
    cmd += list(args)
    return run(cmd, binary=binary, timeout=timeout)


def find_firmware(argv):
    if argv:
        return Path(argv[0])
    return ROOT / "build" / "royalkludge-rk61plus_vial_smk.hex"


def prereqs(fw: Path) -> str | None:
    if sys.platform != "linux":
        return "not Linux"
    if not fw.exists():
        return f"firmware not built: {fw}"
    if not Path("/sys/devices/platform/vhci_hcd.0/attach").exists():
        return "vhci-hcd not loaded"
    if shutil.which("docker") is None and os.geteuid() != 0:
        return "docker not found (needed for the root vhci write)"
    if not Path(USBIP_BIN).exists():
        return f"usbip binary not found: {USBIP_BIN}"
    return None


def wait_port(port: int, timeout: float = 15.0) -> None:
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"port {port} never opened")


class Device:
    def __init__(self, fw: Path):
        self.fw = fw
        self.procs: list[subprocess.Popen] = []
        self.fifo_fd = -1
        self.iface0 = ""
        self.iface1 = ""
        self.event = ""

    def start(self) -> None:
        if os.path.exists(FIFO):
            os.remove(FIFO)
        os.mkfifo(FIFO)
        # O_RDWR so opening the FIFO neither blocks nor gets EOF; ucsim's command
        # console needs an open stdin.
        self.fifo_fd = os.open(FIFO, os.O_RDWR)
        env = dict(os.environ, SMK_UCSIM_HOST=str(UCSIM_PORT))
        self.procs.append(subprocess.Popen(
            [str(ROOT / ".mise/ucsim/bin/ucsim-sh68f90"), "-t", "sh68f90",
             "-e", 'file "%s"' % self.fw, "-g"],
            stdin=self.fifo_fd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env))
        wait_port(UCSIM_PORT)
        time.sleep(0.2)
        self.procs.append(subprocess.Popen(
            [sys.executable, str(ROOT / "tools/usbip_bridge.py"),
             "--ucsim-port", str(UCSIM_PORT), "--listen-port", str(BRIDGE_PORT),
             "--control-port", str(CONTROL_PORT), "--busid", "1-1"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        wait_port(BRIDGE_PORT)
        wait_port(CONTROL_PORT)
        time.sleep(1.0)

    def attach(self) -> None:
        # A root container sharing this kernel supplies root for the vhci sysfs.
        r = docker("--net=host", "-v", "/sys:/sys", "-v", f"{USBIP_BIN}:/usr/bin/usbip:ro",
                   IMAGE, "usbip", "attach", "-r", "127.0.0.1", "-b", "1-1")
        if r.returncode != 0:
            raise RuntimeError(f"usbip attach failed: {r.stdout}{r.stderr}")
        time.sleep(2.0)
        self._locate()

    def _locate(self) -> None:
        for h in Path("/sys/class/hidraw").glob("hidraw*"):
            real = os.path.realpath(h / "device")
            if "1-4:1.0" in real:
                self.iface0 = h.name
            if "1-4:1.1" in real:
                self.iface1 = h.name
        for block in Path("/proc/bus/input/devices").read_text().split("\n\n"):
            if "SMK Keyboard" in block and "kbd" in block:
                m = re.search(r"event(\d+)", block)
                if m:
                    self.event = f"event{m.group(1)}"

    def press_esc(self) -> None:
        s = socket.create_connection(("127.0.0.1", CONTROL_PORT), timeout=5)
        s.sendall(b"press 0 0\n")
        time.sleep(0.5)
        s.sendall(b"release 0 0\n")
        s.close()

    def read_input_event(self) -> bytes:
        if not self.event:
            raise RuntimeError("no keyboard input event device")
        # The reader must be open before the key is pressed: evdev does not
        # replay events that happened before the file was opened. So start the
        # read, press, then collect.
        proc = subprocess.Popen(
            ["docker", "run", "--rm", "-v", "/dev:/dev", "--device-cgroup-rule", "c 13:* rmw",
             IMAGE, "sh", "-c",
             f"timeout 6 dd if=/dev/input/{self.event} bs=24 2>/dev/null"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        self.press_esc()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        return out

    def vial_roundtrip(self) -> bytes:
        if not self.iface0:
            raise RuntimeError("no raw-HID (interface 0) hidraw device")
        script = (
            "printf '\\x00\\xfe\\x00' > /tmp/req; head -c 30 /dev/zero >> /tmp/req; "
            f"( cat /tmp/req > /dev/{self.iface0} ) & sleep 1; "
            f"timeout 3 dd if=/dev/{self.iface0} bs=32 count=1 2>/dev/null"
        )
        r = docker("--device", f"/dev/{self.iface0}", IMAGE, "sh", "-c", script,
                   binary=True, timeout=8)
        return r.stdout

    def stop(self) -> None:
        # Closing the bridge detaches the device (vhci sees the socket close).
        for p in self.procs:
            p.terminate()
        for p in self.procs:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        if self.fifo_fd >= 0:
            os.close(self.fifo_fd)
        try:
            os.remove(FIFO)
        except OSError:
            pass


def parse_events(data: bytes):
    return [struct.unpack_from("<qqHHi", data, i * 24)
            for i in range(len(data) // 24)]


def main(argv: list[str]) -> int:
    fw = find_firmware(argv)
    reason = prereqs(fw)
    if reason:
        print(f"SKIP: {reason}", file=sys.stderr)
        return 2

    dev = Device(fw)
    failures: list[str] = []
    try:
        print("starting ucsim + bridge ...")
        dev.start()
        print("attaching to the kernel ...")
        dev.attach()
        print(f"  enumeration: hidraw0={dev.iface0} hidraw1={dev.iface1} event={dev.event}")

        lsusb = run(["lsusb"]).stdout.lower()
        if VENDOR_PRODUCT not in lsusb:
            failures.append("device not in lsusb")
        if not dev.iface0 or not dev.iface1 or not dev.event:
            failures.append("hidraw/input devices missing")

        print("  typing: press Esc ...")
        events = parse_events(dev.read_input_event())
        if any(etype == 1 and code == 1 and value == 1 for (_s, _u, etype, code, value) in events):
            print("    KEY_ESC press seen")
        else:
            failures.append(f"KEY_ESC not seen (events={events})")

        print("  vial: get_keyboard_id over the kernel hidraw ...")
        reply = dev.vial_roundtrip()
        if len(reply) < 12:
            failures.append(f"short Vial reply: {reply!r}")
        else:
            version = struct.unpack_from("<I", reply, 0)[0]
            uid = reply[4:12]
            if version != 6:
                failures.append(f"Vial version {version} != 6")
            else:
                print(f"    protocol {version}, UID {uid.decode('latin1')!r}")
    except Exception as exc:  # noqa: BLE001
        failures.append(f"exception: {exc!r}")
    finally:
        dev.stop()

    if failures:
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
