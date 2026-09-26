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

// Dynamic-entry store: Vial combos, tap dances and key overrides. The tables
// share one A/B flash sector pair (VIAL_ENTRY_ADDR), separate from the keymap.
// get/set return 0 on success and non-zero on a bad index or an unready store.
typedef struct {
    uint16_t input[4];
    uint16_t output;
} vial_combo_entry_t;

typedef struct {
    uint16_t on_tap;
    uint16_t on_hold;
    uint16_t on_double_tap;
    uint16_t on_tap_hold;
    uint16_t custom_tapping_term;
} vial_tap_dance_entry_t;

typedef struct {
    uint16_t trigger;
    uint16_t replacement;
    uint16_t layers;
    uint8_t  trigger_mods;
    uint8_t  negative_mod_mask;
    uint8_t  suppressed_mods;
    uint8_t  options;
} vial_key_override_entry_t;

// Key override option bits (match vial-qmk).
enum vial_key_override_option {
    vial_ko_enabled = (1 << 7),
};

uint8_t dynamic_keymap_combo_count(void);
int     dynamic_keymap_get_combo(uint8_t index, vial_combo_entry_t *entry);
int     dynamic_keymap_set_combo(uint8_t index, const vial_combo_entry_t *entry);

uint8_t dynamic_keymap_tap_dance_count(void);
int     dynamic_keymap_get_tap_dance(uint8_t index, vial_tap_dance_entry_t *entry);
int     dynamic_keymap_set_tap_dance(uint8_t index, const vial_tap_dance_entry_t *entry);

uint8_t dynamic_keymap_key_override_count(void);
int     dynamic_keymap_get_key_override(uint8_t index, vial_key_override_entry_t *entry);
int     dynamic_keymap_set_key_override(uint8_t index, const vial_key_override_entry_t *entry);

// VIA macro buffer. The buffer is a flat byte array; macros are separated by a
// 0x00 byte. Bytes are the QMK send-string encoding (0x01 prefix, then
// 0x01/0x02/0x03 for tap/down/up followed by a keycode, or 0x04 followed by a
// two-byte delay).
uint8_t  dynamic_keymap_macro_count(void);
uint16_t dynamic_keymap_macro_buffer_size(void);
uint8_t  dynamic_keymap_macro_read_byte(uint16_t offset);
void     dynamic_keymap_macro_get_buffer(uint16_t offset, uint16_t size, uint8_t *out);
void     dynamic_keymap_macro_set_buffer(uint16_t offset, uint16_t size, const uint8_t *in);
void     dynamic_keymap_macro_reset(void);

#endif // VIAL_ENABLE
