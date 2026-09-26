# RK61 Plus Vial: feature backlog and plan

Status of the Vial implementation on the RK61 Plus, and the remaining feature
work with effort and implementation notes.

## Implemented

- Keymap remap: get/set/reset keycode, keymap buffer get/set, layer count.
- QMK settings: tap term, permissive hold, hold on other key press.
- Matrix tester (switch matrix state).
- Unlock/lock, keyboard id, size, definition.
- Layer keys: MO, TG, TO, DF, persistent DF, LM, OSL, LT, TT.
- Custom board keys: BT1/2/3, FX prev/next, speed up/down, brightness up/down.
- Mod-tap (MT): hold applies a modifier, tap sends the keycode.
- One-shot mods (OSM): modifier applies to the next key only.
- Vial dynamic-entry command (0xFE + 0x0D) and the combo engine. Combos are
  editable in the Vial GUI and persist in their own A/B flash sector pair.
- Tap dance (TD) and key overrides, sharing the dynamic-entry store with combos.
- Macros: the VIA macro buffer (0x0C-0x10) and a send-string player.

## Missing features

Ordered by value per effort.

### 1. NKRO — small-medium

- `src/smk/report.h`: USB bitfield 13 bytes (15-byte report, fits EP1) vs the
  RF 20 bytes for the dongle. 13 bytes covers keys 0x00-0x67, everything except
  F13-F24.
- `src/smk/usb.c`: NKRO report id in the vial interface-1 descriptor, and
  `usb_send_nkro` sends a numbered NKRO report on EP1 like the 6KRO report.
- `meson.build`: `nkro: true`.

### 2. More layers (8) — medium

- `meson.build` (rk61plus): `vial_layers: 8`, `vial_keymap_sectors: 4`.
- `src/smk/dynamic_keymap.c`: the store assumes the keymap fits one 512-byte
  sector. Rework it to span sectors:
  - staging buffer sized to `VIAL_KEYMAP_TOTAL` (992 bytes for 8 layers),
  - `store_commit` erases and programs across the sector span,
  - `active_sector` becomes an active copy spanning sectors for A/B,
  - replace the one-sector `_Static_assert` with a span check.
- xdata: 8 layers adds ~480 bytes of staging RAM. The combo store and engine
  already added ~180 bytes, so budget for ~2450 of the 4096.

Keymap sizes: 16 + layers x 61 x 2 bytes. 4 layers = 504 (1 sector), 5-8 =
626-992 (2 sectors per copy), 9-12 = 3 sectors, 13-16 = 4 sectors. With A/B
that doubles the sector count.

### 3. Lighting — medium (VIA GUI, not Vial GUI)

- `src/smk/vial.c`: VIA 0x07/0x08/0x09 set/get/save.
- Value-ID table mapped to effect/brightness/speed, plus a user color slot in
  `led_effect.c`.
- Only reachable from VIA GUI; Vial GUI has no lighting tab.

## Constraints

- Endpoints: EP0 8 bytes, EP1 16 bytes, EP2 64 bytes, fixed in hardware. EP2 is
  taken by the Vial raw-HID (32 bytes), so the keyboard lives on EP1 at 16
  bytes. This is why NKRO needs the smaller bitfield and lighting has no room.
- Flash sectors: 512 bytes. The Vial region grows downward from the settings
  sector at 0xEC00: the keymap store takes 0xE400/0xE600, the dynamic-entry
  store 0xE800/0xEA00 and the macro buffer 0xE000/0xE200, so `code_size` is
  0xE000.
- xdata: 4096 bytes general RAM. The vial target is well under it after the
  combo store, engine and OSM state.
- The simulator tests park on a NOP sled. It lives at 0xF000 (the app images
  leave the bootloader region empty); 0x9000 is now inside the firmware's
  `__code` data and must not be used.

## Dynamic-entry protocol

`0xFE 0x0D <sub-op> <index> ...`. Sub-ops follow vial-qmk:

| Sub-op | Meaning | Request | Reply |
| --- | --- | --- | --- |
| 0x00 | get number of entries | — | out[0..3] = tap dance / combo / key override / alt repeat counts |
| 0x01 | tap dance get | index at in[3] | out[0] = status, out[1..10] = 10-byte entry |
| 0x02 | tap dance set | index at in[3], entry at in[4..13] | out[0] = status |
| 0x03 | combo get | index at in[3] | out[0] = status, out[1..10] = 10-byte entry |
| 0x04 | combo set | index at in[3], entry at in[4..13] | out[0] = status |
| 0x05 | key override get | index at in[3] | out[0] = status, out[1..10] = 10-byte entry |
| 0x06 | key override set | index at in[3], entry at in[4..13] | out[0] = status |

A combo entry is `input[4]` keycodes followed by `output` (all big-endian); a
tap-dance entry is `on_tap, on_hold, on_double_tap, on_tap_hold,
custom_tapping_term`; a key-override entry is `trigger, replacement, layers`
(16-bit) followed by `trigger_mods, negative_mod_mask, suppressed_mods,
options` (8-bit). The counts come from the protocol, not the definition JSON,
so no `vial.json` change is needed.

## Phased plan

Each phase is independently testable on hardware and can commit separately.

### Phase 1: Core keycode features — DONE

Mod-tap (MT) in `tapping.c` and one-shot mods (OSM) in `matrix.c`. No storage,
no protocol, no GUI change.

Milestone: MT and OSM keys work over USB and RF. Covered by
`tests/test_vial.py::TestVialModTap` and `::TestVialOneShotMod`.

### Phase 2: Dynamic-entry plumbing + combos — DONE

`vial_dynamic_entry_op` (count / combo get / combo set), a dedicated A/B flash
entry store in `dynamic_keymap.c`, and the combo engine in `combo.c`.

Milestone: combos editable in Vial GUI; J+K+L -> Enter works. Covered by
`tests/test_vial.py::TestVialCombos`.

### Phase 3: Tap dance + key overrides — DONE

Extend the dynamic-entry store with tap-dance and key-override tables, add the
two engines and the remaining sub-ops (0x01/0x02, 0x05/0x06).

Milestone: tap dance and key overrides editable in Vial GUI. Covered by
`tests/test_vial.py::TestVialTapDance` and `::TestVialKeyOverride`.

### Phase 4: Macros — DONE

VIA 0x0c-0x10, a macro flash store and a non-blocking send-string player with a
main-loop hook.

Milestone: record and play a macro from Vial GUI. Covered by
`tests/test_vial.py::TestVialMacros`.

### Phase 5: More layers (8) — medium

Multi-sector keymap store, `vial_layers: 8`.

Milestone: 8 layers in Vial GUI, keymap still survives reflash.

### Phase 6: NKRO — small-medium

13-byte USB bitfield, NKRO report on EP1, `nkro: true`.

Milestone: NKRO over USB, RF/dongle path unchanged.

### Phase 7: Lighting — medium, optional

VIA 0x07/0x08/0x09 plus a user color slot. VIA GUI only, not Vial GUI.

Milestone: brightness/effect/speed/color controllable from VIA GUI.

Order rationale: phases 1 and 2 are done; phase 3 reuses the phase 2 plumbing;
phases 4, 5, 6 are independent of each other; phase 7 is optional and VIA-only,
so it sits last.
