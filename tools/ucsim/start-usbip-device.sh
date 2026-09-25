#!/usr/bin/env bash
#
# Start the simulated SH68F90 as a real USB device on the Linux USB stack.
#
#   tools/ucsim/start-usbip-device.sh [--no-attach] [firmware.hex]
#
# Runs the firmware in ucsim host mode (tools/ucsim/sh68f90.cc), starts
# tools/usbip_bridge.py and, unless --no-attach is given, loads vhci-hcd and
# attaches the simulated device through USB/IP. Needs root (sudo) and the usbip
# user tools for the attach step; the ucsim + bridge part runs unprivileged.
#
# Environment:
#   SMK_UCSIM        patched simulator binary (default: ucsim-sh68f90 on PATH)
#   SMK_UCSIM_PORT   ucsim host-mode TCP port (default: 3241)
#   SMK_USBIP_BUSID  bus id advertised to the kernel (default: 1-1)
#   SMK_USBIP_CONTROL_PORT  matrix-control port (default: 3242, 0 = off)
#   SMK_USBIP_ATTACH  how to get root for the vhci attach: `sudo` (default) or
#                    `docker` (a root container sharing the host kernel; no sudo)
#   SMK_USBIP_BIN    usbip binary to mount in docker mode
#                    (default: /run/usbipd-win/usbip)
#   SMK_USBIP_IMAGE  container image for docker mode (default: alpine:3.20)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

ATTACH=yes
FW=""
for arg in "$@"; do
    case "$arg" in
        --no-attach) ATTACH=no ;;
        *) FW="$arg" ;;
    esac
done
FW="${FW:-$ROOT/build/royalkludge-rk61plus_default_smk.hex}"

UCSIM="${SMK_UCSIM:-ucsim-sh68f90}"
UCSIM_PORT="${SMK_UCSIM_PORT:-3241}"
BUSID="${SMK_USBIP_BUSID:-1-1}"
CONTROL_PORT="${SMK_USBIP_CONTROL_PORT:-3242}"
PYTHON="${PYTHON:-python3}"

if [[ ! -f "$FW" ]]; then
    echo "firmware not found: $FW" >&2
    echo "build one first: meson compile -C build royalkludge-rk61plus_default_smk.hex" >&2
    exit 1
fi

UCSIM_PID=""
BRIDGE_PID=""
FIFO="$(mktemp -u)"
cleanup() {
    [[ -n "$BRIDGE_PID" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
    [[ -n "$UCSIM_PID" ]] && kill "$UCSIM_PID" 2>/dev/null || true
    exec 9>&- || true
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

# ucsim needs an open stdin (a closed one makes the command console quit). Keep
# a FIFO writer open for the whole run.
mkfifo "$FIFO"
SMK_UCSIM_HOST="$UCSIM_PORT" "$UCSIM" -t sh68f90 \
    -e "file \"$FW\"" -g <"$FIFO" &
UCSIM_PID=$!
exec 9>"$FIFO"

# Wait for the host-mode listen socket.
for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$UCSIM_PORT") 2>/dev/null; then
        exec 3>&- || true
        break
    fi
    sleep 0.1
done
sleep 0.2  # let ucsim notice the probe disconnect before the bridge connects

"$PYTHON" "$ROOT/tools/usbip_bridge.py" \
    --ucsim-port "$UCSIM_PORT" --busid "$BUSID" --control-port "$CONTROL_PORT" &
BRIDGE_PID=$!
sleep 0.5

if [[ "$ATTACH" == yes ]]; then
    if [[ "${SMK_USBIP_ATTACH:-sudo}" == docker ]]; then
        # Docker shares this kernel, so a root container writing the host's
        # vhci sysfs attaches the device to this kernel without sudo.
        USBIP_BIN="${SMK_USBIP_BIN:-/run/usbipd-win/usbip}"
        IMAGE="${SMK_USBIP_IMAGE:-alpine:3.20}"
        docker run --rm --net=host -v /sys:/sys -v "$USBIP_BIN":/usr/bin/usbip:ro "$IMAGE" \
            usbip attach -r 127.0.0.1 -b "$BUSID"
        echo "attached via docker; check with: lsusb -v ; dmesg | tail"
    elif ! command -v usbip >/dev/null 2>&1; then
        echo "usbip user tools not found; skipping attach (simulator + bridge are running)" >&2
    else
        sudo modprobe vhci-hcd
        sudo usbip attach -r 127.0.0.1 -b "$BUSID" || true
        echo "attached; check with: lsusb -v ; dmesg | tail"
    fi
fi

echo "simulated device running (firmware: $FW, busid: $BUSID). Ctrl-C to stop."
wait "$UCSIM_PID"
