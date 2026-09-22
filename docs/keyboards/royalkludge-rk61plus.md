# Royal Kludge RK61 Plus

## Specs

- MCU: BYK916 ([SH68F90A](../platforms/sh68f90.md))
- Layout: 60% ANSI (61 keys)
- Matrix: 5 rows x 14 columns
- Backlight: per-key RGB
- Wireless: BK3632 (BT and 2.4G)
- Switches: hot-swap, plus a power on/off switch and a BLE/2.4G switch
- PCBs: three — a key-switch PCB, a small USB hub (1x USB-C in, USB-C + USB-A out), and the main keyboard PCB
- USB hub: `1a40:0801` (Terminus Technology 4-port USB 2.0 hub; the small hub PCB)
- USB: `258a:00f8` (stock firmware descriptor at `0x665B`); the ISP bootloader enumerates as `0603:1020` (descriptor at `0xF9A8`, strings at `0xF900`)

## Pictures

FCC internal photos of the three PCBs:

| Page 1 | Page 2 | Page 3 |
| --- | --- | --- |
| ![internal photo 1](../../assets/keyboards/royalkludge-rk61plus/internal-1.png) | ![internal photo 2](../../assets/keyboards/royalkludge-rk61plus/internal-2.png) | ![internal photo 3](../../assets/keyboards/royalkludge-rk61plus/internal-3.png) |

Full filing: [FCC ID 2A4MQ-RK61PLUS](https://fccid.io/2A4MQ-RK61PLUS)

## SMK Supported Features

- [x] Key Scan
- [x] Wireless
- [x] RGB Matrix

## Key matrix

Rows read back active-low on P7 and P5; columns are driven low one at a time.

| Row | Pin |
| --- | --- |
| R0 | P7.1 |
| R1 | P7.2 |
| R2 | P7.3 |
| R3 | P5.3 |
| R4 | P5.4 |

| Col | Pin | Col | Pin |
| --- | --- | --- | --- |
| C0 | P6.0 | C7 | P6.7 |
| C1 | P6.1 | C8 | P5.0 |
| C2 | P6.2 | C9 | P5.1 |
| C3 | P6.3 | C10 | P5.2 |
| C4 | P6.4 | C11 | P5.7 |
| C5 | P6.5 | C12 | P4.0 |
| C6 | P6.6 | C13 | P4.2 |

## Wireless (BK3632)

The radio is a BK3632, same part as the NuPhy Air60, but wired to different
pins:

| Signal | Pin |
| --- | --- |
| SCK (clock) | P4.7 |
| MOSI | P0.4 |
| MISO | P0.3 |
| CS | P4.4 |
| WAKE | P0.2 |
| ACK | P4.1 |

The band input (P5.5) selects 2.4G vs BLE; within BLE, the BT channel is chosen
with `Fn`+`Q`/`W`/`E` (`LNK_BT1`/`LNK_BT2`/`LNK_BT3`). A short press switches to
that channel, a long press starts pairing for it, and a short press on the
active, connected channel disables BLE and falls back to USB when a host is
attached. Reports follow the real RF link status: `connected`/`paired` come from
the BK3632 status reply, and USB is the fallback while no RF link is actually
connected. Pressing a BT channel key re-enables RF. On the direct 2.4G band the
`Fn`+`Q`/`W`/`E` BLE keys are disabled.

The `Fn`+`Q`/`W`/`E` indicator shows the active channel: solid blue when
connected, slow blink when the channel is selected but no link is up (paired or
not), and fast blink only while a pairing sequence is running.

## USB

The USB stack is stock-aligned where it matters for enumeration. The stock
commits the device address in the EP0 IN (status-stage) handler, not in the
setup handler: `Function_118` at `0xA8FE` is reached from the `IEP0IF`
interrupt and does `MOV USBADDR,A` at `0xA941`. It also does not stall EP0 OUT
for SET_ADDRESS (the only `OEP0STL` writes are error paths). SMK matches this:
`usb_set_address_handler()` records `received_usb_addr` and arms the status
stage, and `usb_ep0_in_irq()` commits `USBADDR` there. The one divergence was a
spurious `SET_EP0_OUT_STALL` in the setup handler, which could leave EP0 OUT
stalled into the next SETUP and intermittently drop SET_CONFIGURATION (`can't
set config #1, error -32`); it has been removed.

`usb_hw_init()` matches the stock USB init (`0xB144`): `USBADDR=0`, `USBIE1=0x5F`,
`USBIE2=0x11`, `USBCON=0xC0`.

## Battery monitoring

The SH68F90 has no ADC. The stock firmware measures the battery with a
bit-banged RC-timing loop on P0.0/P0.1: drive a pin low to discharge a
capacitor, release it, then time how long the other pin takes to flip — the
classic ADC substitute. It runs at every boot/wake (stock `0xF000` → `f770`),
kicks the watchdog while it measures, and stores the result in flag bit `0x04`.

SMK implements the same RC-timing path in `user_battery.c`, run at boot and on
wake: P0.1 discharges the capacitor and then charges it through its pull-up, and
a count loop (kicking the watchdog each iteration) times how long P0.0 stays high
before it flips. The count maps to `keyboard_state.battery_level` (0..7) and a
1-bit `low_power` flag (level ≤ 1). The 0..7 split is a first-order linear split
of the stock's 100-count window (`0x0E = 0x64`); the exact RC time constant is
board-dependent, so the thresholds need a hardware calibration pass.

## Other pins

| Pin | Stock role | Boot | Park |
| --- | --- | --- | --- |
| P0.5, P4.3, P4.5, P4.6, P7.4 | enable group — hub enable + charge enable + 3 more | 1 | 0 |
| P0.0, P0.1 | RC battery measurement | 0 | 0 |
| P0.6, P0.7 | status inputs (`0x309E` / `0x30A2`) | input | 0 |
| P7.0 | output, parked only | 0 | 0 |
| P7.5 | status input | input | input (never driven) |
| P7.6, P7.7 | control/status (`0x7B63` / `0x7D32` / `0xF9F0`) | 0 | 0 |
| P1.6/7, P2.6/7, P3.6/7 | NC / unbonded | — | — |

Boot values come from `MOV P0,#24` / `MOV P4,#FD` / `MOV P7,#10`; park values
from `ANL P0,#1F` / `ANL P0,#FC`, `ANL P4,#92`, `ANL P7,#EE` / `ANL P7,#3F`
(plus the full clear at `0xFC5F`). The five enable pins are driven high at boot
and low at park as one group — hub and charging are separate wires, but the
firmware never gates them individually.

## Switches

The mode is read from two active-low inputs (pull-ups enabled), mapped on
hardware:

| Input | Pin | Levels | Meaning |
| --- | --- | --- | --- |
| band | P5.6 | B = high, G = low | B → BLE, G → 2.4G |
| on/off | P5.5 | on = low, off = high | off → wired/USB, on → wireless |

The two inputs select the mode: on/off **off** → wired/USB (the cable powers the
board); on/off **on** + B → BLE (last BT channel); on/off **on** + G → direct 2.4G.
The stock switch-poll trace (`0x7C00`) in the original plan had P5.5/P5.6 swapped;
the hardware mapping above was confirmed by flipping the switches and reading the
pins. The radio stays up in wired/USB mode too, so `Fn`+`Q`/`W`/`E` can connect a
BLE host while the cable is plugged in and toggle back to USB (stock behaviour).

## Timing

The main loop (and `kb_update()`) runs at roughly **100 Hz** (~10 ms per tick) on
this board — measured by logging a tick counter (1000 ticks took ~10 s). Several
timeouts are counted in those ticks, so they are easy to mis-size:

| Constant | Ticks | Real time | Meaning |
| --- | --- | --- | --- |
| `LINK_PAIRING_HOLD_TICKS` | 300 | ~3 s | `Fn`+`Q`/`W`/`E` long-press starts pairing |
| `SLIDER_DEBOUNCE_ITERS` | 256 | ~2.6 s | band / on-off switch debounce |
| `RF_SUPERVISOR_TICK_INTERVAL` | 500 | ~5 s | RF status poll interval |
| `RF_PAIRING_WINDOW_POLLS` | 600 | ~50 min | pairing window before re-asserting the link |

The RF status is polled only every ~5 s, so connect/disconnect shows up with up
to that latency, and the pairing window is effectively unbounded. The original
`LINK_PAIRING_HOLD_TICKS = 60000` was ~10 min, which is why a long-press never
armed pairing.

The LED side is driven by Timer2 instead of the main loop: ~100 us per matrix
scan and ~400 us per LED subframe (14 subframes per frame → ~5.6 ms/frame). The
channel indicator blink masks count those frames: `FN_BLINK_FAST = 0x10`
(~4-5 Hz) and `FN_BLINK_SLOW = 0x40` (~1 Hz).

## Backlight

The backlight is a per-key RGB matrix driven by the SH68F90's PWM units. It is
the transpose of the NuPhy Air60 matrix: the PWM channels are the row/colour
sinks and the key-matrix columns are the LED columns.

| Side | Pins |
| --- | --- |
| Row/colour sinks (PWM) | P1.0-5 (PWM00-05), P2.0-5 (PWM10-15), P3.0-5 (PWM20-25) |
| LED columns | P6.0-7, P5.0-2, P5.7, P4.0, P4.2 |

Each keyboard row owns three consecutive PWM channels, in the order green, red,
blue:

| Row | Pins | Green | Red | Blue |
| --- | --- | --- | --- | --- |
| Ctrl (R4) | P1.0-2 | PWM00 | PWM01 | PWM02 |
| Shift (R3) | P1.3-5 | PWM03 | PWM04 | PWM05 |
| Tab (R1) | P2.0-2 | PWM10 | PWM11 | PWM12 |
| Caps (R2) | P2.3-5 | PWM13 | PWM14 | PWM15 |
| spare (unconnected) | P3.0-2 | PWM20 | PWM21 | PWM22 |
| Esc (R0) | P3.3-5 | PWM23 | PWM24 | PWM25 |

PWM bank 4 (PWM40-42) is unused.

Driving (traced from the stock firmware): the PWM registers are memory-mapped
(`MOVX @DPTR` at `0xFF80`-`0xFFF9`). Init (`0x6B14`) sets `CON`, `PER` (`0x0438`)
and a per-channel `DUTY1 = DUTY2` test ramp (`0x35, 0x36, ...`); the render paths
(`0x5xxx`, `0x5fxx`, `0x6bxx`, `0x97xx`) read the XRAM LED framebuffer and write
it to **DUTY2** — so DUTY2 is the animated duty and DUTY1 is the fixed on-time.

Because the LED columns share the key-matrix column pins, the matrix scan and the
LED scan must be time-multiplexed. The LED columns are **active-low**: the
selected column is driven LOW to source current into its row's sinks, and the
other columns idle HIGH. (The original trace had this inverted, which lit the
whole row except the selected key.)

Backlight and function controls follow the RK61 Plus manual:

| Keys | Action |
| --- | --- |
| `Fn`+`\` | cycle the RGB effect |
| `Fn`+`[` / `Fn`+`]` | brightness down / up |
| `Fn`+`;` / `Fn`+`'` | animation speed down / up |
| `Fn`+`Y` / `U` / `I` | PrtSc / ScrLK / Pause |
| `Fn`+`H` / `J` / `K` | Insert / Home / PgUp |
| `Fn`+`N` / `M` / `,` | Del / End / PgDn |
| `Fn`+`/` / `RAlt` / `Menu` / `RCtrl` | arrows Up / Left / Down / Right |

The GPIO pins P4.3, P4.5, P4.6, P0.5, P7.4 are not part of the RGB matrix (no
PWM channel maps to them) — they are driven in the "park" routine and are likely
USB-hub/charging control lines.

## Code Options

TBD — not yet traced on this board.
