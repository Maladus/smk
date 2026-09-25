# Simulated USB device on the Linux USB stack

Runs the firmware in the patched ucsim simulator and attaches it to the Linux
kernel as a real USB device through USB/IP, so the kernel, hidraw and the Vial
GUI talk to the simulated firmware. Linux only: the simulator host mode and the
USB/IP bridge need the Linux USB/IP stack. Windows and macOS are covered by
hardware tests instead.

## Pieces

- `sh68f90.cc` — the ucsim SH68F90 CPU variant. Besides the existing test mode
  (driven by `tests/sim.py`) it has a host mode, enabled with
  `SMK_UCSIM_HOST=<port>`, that serves framed SIE transactions over TCP:
  `RESET`, `SETUP`, `OUT` data, `IN` request (data / NAK / STALL), `SOF`, an
  idle handshake (`WAIT`), and matrix helpers (`PINS`, `GET_SFR`, `KEY`). The
  test mode is untouched when the variable is unset.
- `../usbip_bridge.py` — USB/IP server (standard library only). Turns kernel
  URBs into SIE transactions and keeps interrupt IN URBs pending until the
  firmware sets `IEPnRDY`, like the real device NAKing until it has data. It
  also waits for the simulated firmware to finish each control transfer
  (`WAIT`) before starting the next, sends the `SET_ADDRESS` the kernel's vhci
  never forwards (the SMK firmware only accepts `SET_CONFIGURATION` once
  addressed), and exposes a local matrix-control port so a test can type
  through the emulated key matrix.
- `start-usbip-device.sh` — starts ucsim and the bridge, then loads
  `vhci-hcd` and runs `usbip attach` (needs root and the `usbip` user tools).
- `host_selftest.py` — the same path without root: starts ucsim and the bridge,
  drives the bridge with a minimal fake-vhci USB/IP client and checks
  enumeration, a Vial round trip on a Vial image, and a key typed through the
  emulated matrix (Esc on EP1).

## Build the simulator

```
mise run setup-ucsim          # or: nix build .#ucsim-sh68f90
```

## Run

```
meson compile -C build royalkludge-rk61plus_default_smk.hex
tools/ucsim/start-usbip-device.sh build/royalkludge-rk61plus_default_smk.hex
```

`--no-attach` skips the privileged part and only runs ucsim and the bridge.
The start script waits for Ctrl-C; the firmware keeps running in ucsim until
then.

If you have neither root nor `sudo`, run the attach from a root container that
shares this kernel and bind-mounts the host `/sys` (no `--privileged` needed):

```
SMK_USBIP_ATTACH=docker tools/ucsim/start-usbip-device.sh build/royalkludge-rk61plus_vial_smk.hex
```

The device then shows up in `lsusb`/`dmesg`, its keyboard becomes a real
`/dev/input/event*`, and the matrix-control port can drive it. Reading the
input/hidraw nodes from a container needs the device cgroup to allow them, e.g.
`--device-cgroup-rule='c 13:* rmw'` for input and `'c 240:* rmw'` for hidraw.

Without root (or without the `usbip` tools) the components still run:

```
tools/ucsim/host_selftest.py build/royalkludge-rk61plus_vial_smk.hex
```

This is also the regression check for the simulator and bridge changes: it
fails if enumeration, control transfers or the pending interrupt-IN path break.

## Environment

- `SMK_UCSIM` — patched simulator binary (default
  `.mise/ucsim/bin/ucsim-sh68f90`; also installed on `PATH` by `mise`).
- `SMK_UCSIM_HOST` — ucsim host-mode TCP port, set by the start script.
- `SMK_UCSIM_PORT` — ucsim port used by the start script (default `3241`).
- `SMK_USBIP_BUSID` — bus id advertised to the kernel (default `1-1`).
- `SMK_UCSIM_HOST_DEBUG` — dump the event ring on an EP0 stall.
- `SMK_UCSIM_HOST_DEBUG_VERBOSE` — log every host-mode transaction to stderr.

## Typing through the matrix

The bridge can stage keys in the simulator's emulated RK61 Plus matrix over a
local control port (`--control-port`, default off; the selftest uses 3273).
Connect and send `press <row> <col>`, `release <row> <col>` or `clear`:

```
printf 'press 0 0\n' | nc 127.0.0.1 3273   # Esc down
```

The simulator recomputes the row pin levels on every P5/P7 read, so the
firmware's scan sees exactly the pressed keys and sends a normal keyboard
report on EP1.

## Limits

Host mode models the SIE and the RK61 Plus matrix, not the silicon, so hardware
checks stay. It is a local tool; CI can adopt it once the runners provide
`vhci-hcd` and the usbip user tools. The kernel-attach step itself needs root
and a working `/dev/vhci`, so on a host without them only `host_selftest.py`
runs.
