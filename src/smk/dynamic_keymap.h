#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Dynamic keymap store in program flash.
//
// Two 512-byte sectors just below the settings sector hold one copy each: a
// header (magic, version, layer count, key table hash, checksum, saved base
// layer, tapping term and hold flags) followed by the keymap payload. Only the
// keys a board really has are stored; the generated table maps a matrix
// position to its stored slot. Reads go straight to flash, writes stage a new
// copy into the spare sector and switch to it, so an interrupted write always
// leaves the previous copy intact.
//
// `dynamic_keymap_init()` is called from matrix_init(): it picks the newest
// valid copy or reseeds the defaults when neither is usable (a changed layer
// count or key table invalidates the store on purpose).

void dynamic_keymap_init(void);

// Raw keymap access for the layer engine: (layer, row, col) -> keycode. Empty
// matrix positions read as KC_NO.
uint16_t dynamic_keymap_get(uint8_t layer, uint8_t row, uint8_t col);

// VIA flat keymap access. `offset` is a byte offset into the layer-major,
// row-major keymap (each keycode two bytes, big-endian). `size` is a byte count.
uint16_t dynamic_keymap_get_offset(uint16_t offset);
void     dynamic_keymap_set_offset(uint16_t offset, const uint8_t *keycodes_be, uint8_t size);

// Vial dynamic keymap set/reset for one key.
void dynamic_keymap_set_keycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);
void dynamic_keymap_reset_keycode(uint8_t layer, uint8_t row, uint8_t col);

// Persist a new base layer (PDF). `matrix.c` calls this only when it changes.
void dynamic_keymap_save_base_layer(uint8_t layer);

// QMK settings backing store, read by the tapping engine at runtime.
uint16_t dynamic_keymap_tapping_term(void);
void     dynamic_keymap_set_tapping_term(uint16_t term);
bool     dynamic_keymap_permissive_hold(void);
void     dynamic_keymap_set_permissive_hold(bool on);
bool     dynamic_keymap_hold_on_other_key_press(void);
void     dynamic_keymap_set_hold_on_other_key_press(bool on);
void     dynamic_keymap_reset_settings(void);

#endif // VIAL_ENABLE
