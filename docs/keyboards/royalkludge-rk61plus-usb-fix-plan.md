# RK61 Plus: reliable USB on plug-in — implementation plan

**Repo/branch:** `~/work/private/smk`, branch `rk61plus`
**Goal:** The keyboard must enumerate and work over USB reliably every time it is
plugged in (no intermittent `can't set config #1, error -32`), and boot should be
fast (backlight immediate). Align the USB enumeration path with the stock firmware
dump (baseline).

## State when this plan starts

- Working tree is at HEAD; two stashes exist:
  - `stash@{0}` — **"transport+boot changes (WIP)"** — the connection fix (keep,
    with one edit).
  - `stash@{1}` — "rk61plus bring-up WIP: switch pin swap, pairing hold fix,
    debug logging" — unrelated, leave alone.
- `stash@{0}` contains the transport + boot changes **and** a temporary debug
  `dprintf("mode: 24g=%u wired=%u\r\n", ...)` in `kb.c` `kb_apply_band` that must
  be removed.
- Reference dump:
  `assets/keyboards/royalkludge-rk61plus/reference/rk61plus-stock-full.hex`
  (full image; app + bootloader at `0xF000`).

## Root cause (from the dump + hardware logs)

1. Enumeration is **intermittent**: `Device not responding to setup address` →
   SET_ADDRESS never lands → `received_usb_addr == 0` →
   `usb_set_configuration_handler()` STALLs (`can't set config #1, error -32`).
2. Stock baseline (`0xAF0D` init → `0xB144` USB → `0xB096` enable IRQs → loop;
   RF from the loop) commits the address **inside the SET_ADDRESS handler**:

   ```
   0xA934  MOV DPTR,#0x08a8      ; setup packet
   0xA937  MOVX A,@DPTR          ; state
   0xA938  JNZ  0xA943
   0xA93A  INC  DPTR
   0xA93B  MOVX A,@DPTR          ; bRequest
   0xA93C  CJNE A,#0x05,0xA943   ; SET_ADDRESS
   0xA93F  INC  DPTR
   0xA940  MOVX A,@DPTR          ; wValue
   0xA941  MOV  USBADDR,A        ; commit here
   0xA943  RET
   ```

   SMK instead defers the commit to `usb_ep0_in_irq()` and also does
   `SET_EP0_OUT_STALL` in the handler — the divergence to fix.

## Changes

### 1. `src/smk/usb.c` — SET_ADDRESS stock-style (the fix)

- `usb_set_address_handler()`:
  - commit `USBADDR = req->wValue` directly (stock `0xA941`);
  - remove `SET_EP0_OUT_STALL`;
  - keep `CLEAR_EP0_CNT; SET_EP0_IN_RDY;` for the status stage.
- `usb_ep0_in_irq()` default branch: remove the now-redundant
  `USBADDR = received_usb_addr;` (and the `SET_EP0_IN_STALL;` if it isn't
  stock-aligned).
- Keep `received_usb_addr` bookkeeping (used by `usb_set_configuration_handler`).
- Note: `usb.c` is shared with nuphy-air60 — `tests/test_usb.py` is the
  regression guard.

### 2. Keep the boot change from `stash@{0}` (already implemented)

- `src/main.c`: RF bring-up deferred into the main loop (first pass);
  `restore_rf_link()` no longer sets `connected`/`paired`.
- `src/peripherals/bk3632/rf_controller.c`: removed the six `delay_ms(255)`;
  `RF_SUPERVISOR_TICK_INTERVAL 500u`; `supervisor_ticks` seeded to poll on the
  first `kb_update()`.

### 3. Keep the transport change from `stash@{0}`

- `src/keyboards/royalkludge-rk61plus/kb.c`: `kb_set_link()` no longer fakes
  `connected`/`paired`; `kb_wired()` (P5.6); `kb_apply_band()` selects 2.4G / BLE /
  wired-USB (mirrors stock `0x7C00`); `kb_update_switches()` debounces both inputs.
- `src/keyboards/royalkludge-rk61plus/kbdef.h`: `POWER_SWITCH` → `WIRED_SWITCH`
  (P5.6, low = wired/USB).
- **Remove** the temporary `dprintf("mode: ...")` in `kb_apply_band`.

### 4. Tests

- `tests/test_rk61_rf.py` — already added in `stash@{0}` (transport/boot/switch
  coverage). Keep.
- `tests/test_usb.py` — **add** a SET_ADDRESS → SET_CONFIGURATION sequence: assert
  `USBADDR == wValue`, `received_usb_addr != 0`,
  `usb_device_state == CONFIGURED` (harness already boots through init).
- Run full `mise run usim`.

### 5. Docs

- `docs/keyboards/royalkludge-rk61plus.md`: note stock-aligned SET_ADDRESS/EP0
  handling; already has the Wireless/Switches updates from `stash@{0}`.

## Execution order

1. `git stash pop stash@{0}` (restores the connection fix).
2. Edit `kb.c` to remove the temporary `mode:` dprintf.
3. Apply change 1 (`usb.c` SET_ADDRESS).
4. Add the `test_usb.py` case.
5. `mise run build royalkludge-rk61plus_default_smk.hex` and `mise run usim`
   (expect 60+ tests OK).
6. Hardware verify (below).
7. Commit.

## Hardware verification

- Flash over USB ISP: `mise run flash-usb`.
  - If `sinowisp` reports `Device not found` / `Permission denied` on
    `/dev/hidraw*`: the ISP interface is iface 1. Either replug until `hidraw1`
    exists, or trigger the bootloader with a raw feature report and re-run the
    flash:

    ```python
    # SET_REPORT(feature, id=5) to interface 1 with payload [0x05,0x75]
    # -> ISP bootloader (0603:1020)
    dev.ctrl_transfer(0x21, 0x09, 0x0305, 1, [0x05, 0x75])
    ```

    then `mise run flash-usb`.
- Use a **known-good USB port** (the host xHCI can wedge after many
  re-enumerations; `journalctl -k` shows
  `invalid context state for evaluate context command`). If it wedges, try
  another port / reboot the host.
- Repeat plug-in several times; confirm every time:
  - `lsusb | grep 258a` shows `258a:00f8`, `bConfigurationValue=1`, and
    `/dev/hidraw0` + `/dev/hidraw1` exist.
  - No `can't set config #1, error -32` in `journalctl -k`.
- Type immediately after plug-in: keystrokes land over USB with no 5–10 s delay;
  boot backlight is immediate.

## Acceptance criteria

- Repeated plug-ins on one healthy port enumerate reliably (`cfg=1`,
  `hidraw0`+`hidraw1`), no `-32`.
- Typing works over USB from the first keypress; boot backlight immediate.
- `mise run build` + `mise run usim` green (nuphy + rk61).
- `clang-format --dry-run --Werror` clean on changed C/H.

## Reference facts

- Stock USB init (`0xB144`): `USBADDR=0`, `USBIE1=0x5F`, `USBIE2=0x11`,
  `USBCON=0xC0` — SMK's `usb_hw_init()` already matches bit-for-bit.
- Stock boot: `0x7F70` kick → `0x7F73 LCALL 0xAF0D` (init incl. USB) → `0x7F7A`
  enable IRQs → delay loop → `0x7FA2` PWM init → loop.
- Stock mode select `0x7C00`: P5.5 high → 2.4G; P5.5 low + P5.6 high → BLE;
  P5.5 low + P5.6 low → USB.
- Board: RK61 Plus, BYK916/SH68F90A, BK3632 radio, USB `258a:00f8`, ISP
  bootloader `0603:1020`.
