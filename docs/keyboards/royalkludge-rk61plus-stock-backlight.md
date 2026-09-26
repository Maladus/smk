# RK61 Plus stock firmware: backlight reverse-engineering notes

Findings from disassembling the stock dump, focused on the backlight and what
it implies for the SMK Vial build.

Dump: `assets/keyboards/royalkludge-rk61plus/reference/rk61plus-stock-full.hex`

## How to disassemble

Use the patched simulator with the `sh68f90` CPU variant, load the `.hex`, and
dump with `dc`:

```
.mise/ucsim/bin/ucsim-sh68f90 -t sh68f90 -q -c -
file "/home/martin/work/private/smk/assets/keyboards/royalkludge-rk61plus/reference/rk61plus-stock-full.hex"
dc 0x5f00 0x5f80
quit
```

Raw bytes with `dump rom <start> <stop>`.

## Confirmed

### Reset and interrupt vectors

- 0x0000 `LJMP 0x9db5` reset
- 0x0003 `LJMP 0xa45a`
- 0x000b / 0x0013 / 0x0016 / 0x001b the other interrupt entries

### PWM init (0x6b14)

Walks the memory-mapped PWM registers at 0xff81-0xfff9, sets CON and PER
(0x0438), writes a DUTY1=DUTY2 ramp (0x35, 0x36, 0x37, ...) across the 15
row/colour channels. Matches the documented model: 15 channels, animated duty
on DUTY2, DUTY1 is the fixed on-time.

### The framebuffer is per-key RGB

The render path at 0x5ec0-0x5f80 indexes into XRAM with a key index (internal
RAM 0x12), multiplies by 2, and reads colour planes, then writes each to the
PWM DUTY2 registers (0xffd7-0xfff9).

Channel planes (base + key index * 2):

- 0x0375
- 0x039f
- 0x049b
- 0x056d

So the stock holds a per-key colour framebuffer in XRAM, and "render" is a
framebuffer -> PWM copy. Effects are procedural writers into that framebuffer,
not part of the PWM stage. Four planes for RGB suggests a 16-bit-per-channel or
split-DUTY layout; the exact pixel format is still to decode.

### Settings/state block

There is a settings/state block in XRAM around 0x086c-0x087f. Seen so far:

- 0x086c: written to zero in a 0x7f00 routine that then indexes a code table at
  0x020d by `value * 2`.
- 0x0871/0x0872: a 16-bit value compared against 0x007e.
- 0x0873, 0x0878, 0x0879, 0x087a, 0x087b, 0x087c: read heavily in a boot
  validation routine (0x0200-0x022f) that checks magic bytes.

This block is the likely home of the effect index, brightness, speed and
possibly a saved custom pattern. Needs a pass to map each byte.

## Implications for SMK

- Per-key custom colour is already the stock architecture. A "custom" effect in
  `led_effect.c` would read a stored per-key table instead of computing
  procedurally. 61 keys * 3 bytes = 183 bytes, fits one flash sector.
- Per-layer lighting is the same table replicated per Vial layer, selected by
  the active layer. The stock has one framebuffer; the per-layer variant is an
  SMK extension but the hardware supports it.
- The SMK `led_fb[row][channel][col]` framebuffer already mirrors this design.

## Still to find

- The exact effect index byte and the effect table, to enumerate the stock
  effects (count and names).
- Whether the stock has a user paint/record mode for the framebuffer, or only
  preset effects.
- The pixel format (what the four planes and the 2-byte stride mean).
- How Fn+\\ cycles the effect (the key handler that increments the effect
  index).

## Method note

The `dc` output marks unmapped addresses with `?` and disassembles data tables
as code, so a pointer table must be read with `dump rom`, not `dc`. The table
at 0x020d looked like code under `dc`; confirm it as data before trusting it.
