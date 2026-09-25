#include "vial.h"
#include "dynamic_keymap.h"
#include "kbdef.h"
#include "keycodes.h"
#include "layout.h"
#include "matrix.h"
#include "tick.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef VIAL_ENABLE

// Vial / VIA protocol handler.
//
// Command ids and framing follow vial-qmk (quantum/via.c, quantum/vial.c). The
// transport is 32-byte raw-HID reports (usb.c).
//
// Two reply conventions, matching the reference:
//   - VIA commands (no prefix): the reply data starts at out[1]; out[0] echoes
//     the command id (the reference edits the received buffer in place).
//   - Vial commands (0xFE prefix): the reply starts at out[0].

// VIA command IDs
#    define CMD_VIA_GET_PROTOCOL_VERSION  0x01
#    define CMD_VIA_GET_KEYBOARD_VALUE    0x02
#    define CMD_VIA_SET_KEYBOARD_VALUE    0x03
#    define CMD_VIA_GET_KEYCODE           0x04
#    define CMD_VIA_SET_KEYCODE           0x05
#    define CMD_VIA_RESET_KEYCODE         0x06
#    define CMD_VIA_MACRO_GET_COUNT       0x0C
#    define CMD_VIA_MACRO_GET_BUFFER_SIZE 0x0D
#    define CMD_VIA_GET_LAYER_COUNT       0x11
#    define CMD_VIA_KEYMAP_GET_BUFFER     0x12
#    define CMD_VIA_KEYMAP_SET_BUFFER     0x13

// VIA get_keyboard_value sub-commands
#    define VIA_UPTIME              0x01
#    define VIA_LAYOUT_OPTIONS      0x02
#    define VIA_SWITCH_MATRIX_STATE 0x03

// Vial commands, all behind CMD_VIA_VIAL_PREFIX
#    define CMD_VIA_VIAL_PREFIX         0xFE
#    define CMD_VIAL_GET_KEYBOARD_ID    0x00
#    define CMD_VIAL_GET_SIZE           0x01
#    define CMD_VIAL_GET_DEFINITION     0x02
#    define CMD_VIAL_GET_UNLOCK_STATUS  0x05
#    define CMD_VIAL_UNLOCK_START       0x06
#    define CMD_VIAL_UNLOCK_POLL        0x07
#    define CMD_VIAL_LOCK               0x08
#    define CMD_VIAL_QMK_SETTINGS_QUERY 0x09
#    define CMD_VIAL_QMK_SETTINGS_GET   0x0A
#    define CMD_VIAL_QMK_SETTINGS_SET   0x0B
#    define CMD_VIAL_QMK_SETTINGS_RESET 0x0C
#    define CMD_VIAL_DYNAMIC_ENTRY_OP   0x0D

#    define VIAL_DYNAMIC_ENTRY_GET_NUMBER_OF_ENTRIES 0x00

#    define VIA_UNHANDLED 0xFF

// The VIA version the host checks (vial-gui supports 9).
#    define VIA_PROTOCOL_VERSION 0x0009

// Vial protocol version. The host accepts the versions it knows and fetches the
// keyboard definition fresh on every connect, so a definition-only change must
// not bump this.
#    define VIAL_PROTOCOL_VERSION 6

// Keyboard UID: Vial keys per-board data off this, so it must be stable and
// unique. Fixed 8 bytes, "RK61Plus".
static const __code uint8_t vial_uid[8] = {0x52, 0x4b, 0x36, 0x31, 0x50, 0x6c, 0x75, 0x73};

// The const keymap this board ships (base + Fn) is defined by the layout; the
// dynamic store seeds from it and layers past it start transparent.

// The definition is baked from the board's vial.json at configure time (meson
// `vial_def_gen.py`); without one a minimal placeholder keeps the build going.
#    ifdef VIAL_DEFINITION_HEADER
#        include VIAL_DEFINITION_HEADER
#    else
static const __code char    vial_definition[]                   = "{\"name\":\"RK61 Plus\",\"vendorId\":\"0x258A\",\"productId\":\"0x00F8\","
                                                                  "\"matrix\":{\"rows\":5,\"cols\":14},\"layouts\":{\"keymap\":[]}}";
#        define VIAL_DEFINITION_SIZE (sizeof(vial_definition) - 1)
#        define VIAL_KEY_TABLE_SIZE  (MATRIX_ROWS * MATRIX_COLS)
#        define VIAL_NUM_KEYS        0
static const __code uint8_t vial_key_table[VIAL_KEY_TABLE_SIZE] = {0};
#    endif

// The definition is served in 32-byte blocks (Vial's vial_get_def).
#    define VIAL_DEF_BLOCK 32

// QMK settings (Vial QMK Settings tab).
#    define QMK_SETTING_TAPPING_TERM            7
#    define QMK_SETTING_PERMISSIVE_HOLD         22
#    define QMK_SETTING_HOLD_ON_OTHER_KEY_PRESS 23

// Unlock: Vial refuses to write until the host completes an unlock handshake.
// VIAL_INSECURE-style: accept immediately.
static uint8_t vial_unlocked;
static uint8_t vial_unlock_in_progress;

_Static_assert(VIAL_KEY_TABLE_SIZE == MATRIX_ROWS * MATRIX_COLS, "vial key table size must match the matrix");

// Key table accessors for the dynamic store (dynamic_keymap.c).
uint8_t vial_key_slot(uint8_t pos)
{
    if (pos >= VIAL_KEY_TABLE_SIZE) {
        return 0xFF;
    }
    return vial_key_table[pos];
}

// Identifies the generated table so a firmware with a changed layout never
// reads a stale keymap store.
uint8_t vial_key_table_hash(void)
{
    uint8_t hash = 0x5Au;
    for (uint16_t i = 0; i < VIAL_KEY_TABLE_SIZE; i++) {
        hash = (uint8_t)(hash * 31u + vial_key_table[i]);
    }
    return hash;
}

static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// --- Vial commands (reply at out[0]) --------------------------------------

static void vial_get_keyboard_id(uint8_t *out)
{
    wr32le(out, VIAL_PROTOCOL_VERSION);
    for (uint8_t i = 0; i < sizeof(vial_uid); i++) {
        out[4 + i] = vial_uid[i];
    }
}

static void vial_get_size(uint8_t *out)
{
    wr32le(out, VIAL_DEFINITION_SIZE);
}

// The host asks for 32-byte blocks by index; reply is the block at out[0..].
static void vial_get_definition(const uint8_t *in, uint8_t *out)
{
    const uint32_t block = rd32le(in + 2);
    const uint32_t start = block * VIAL_DEF_BLOCK;
    if (start >= VIAL_DEFINITION_SIZE) {
        return;
    }
    uint32_t end = start + VIAL_DEF_BLOCK;
    if (end > VIAL_DEFINITION_SIZE) {
        end = VIAL_DEFINITION_SIZE;
    }
    for (uint32_t i = 0; i < end - start; i++) {
        out[i] = vial_definition[start + i];
    }
}

static void vial_get_unlock_status(uint8_t *out)
{
    for (uint8_t i = 0; i < VIAL_REPORT_SIZE; i++) {
        out[i] = 0xFF;
    }
    out[0] = vial_unlocked;
    out[1] = vial_unlock_in_progress;
}

static void vial_unlock_poll(uint8_t *out)
{
    out[0] = vial_unlocked;
    out[1] = vial_unlock_in_progress;
    out[2] = 0;
}

// id_qmk_settings_query: reply is the list of supported qsids as little-endian
// 16-bit values, 0xFFFF-terminated (and padded).
static void vial_qmk_settings_query(uint8_t *out)
{
    static const __code uint16_t qsids[] = {QMK_SETTING_TAPPING_TERM, QMK_SETTING_PERMISSIVE_HOLD, QMK_SETTING_HOLD_ON_OTHER_KEY_PRESS};
    uint8_t                      i       = 0;
    for (uint8_t n = 0; n < sizeof(qsids) / sizeof(qsids[0]); n++) {
        out[i++] = (uint8_t)(qsids[n] & 0xFF);
        out[i++] = (uint8_t)(qsids[n] >> 8);
    }
    while (i < VIAL_REPORT_SIZE) {
        out[i++] = 0xFF;
    }
}

// id_qmk_settings_get: qsid (LE16) at in[2..3], value at out[4..].
static void vial_qmk_settings_get(const uint8_t *in, uint8_t *out)
{
    const uint16_t qsid = rd16le(in + 2);
    switch (qsid) {
        case QMK_SETTING_TAPPING_TERM: {
            const uint16_t term = dynamic_keymap_tapping_term();
            out[4]              = (uint8_t)(term & 0xFF);
            out[5]              = (uint8_t)(term >> 8);
            break;
        }
        case QMK_SETTING_PERMISSIVE_HOLD:
            out[4] = dynamic_keymap_permissive_hold() ? 1 : 0;
            break;
        case QMK_SETTING_HOLD_ON_OTHER_KEY_PRESS:
            out[4] = dynamic_keymap_hold_on_other_key_press() ? 1 : 0;
            break;
        default:
            break;
    }
}

// id_qmk_settings_set: qsid (LE16) at in[2..3], value at in[4..].
static void vial_qmk_settings_set(const uint8_t *in)
{
    const uint16_t qsid = rd16le(in + 2);
    switch (qsid) {
        case QMK_SETTING_TAPPING_TERM:
            dynamic_keymap_set_tapping_term(rd16le(in + 4));
            break;
        case QMK_SETTING_PERMISSIVE_HOLD:
            dynamic_keymap_set_permissive_hold(in[4] != 0);
            break;
        case QMK_SETTING_HOLD_ON_OTHER_KEY_PRESS:
            dynamic_keymap_set_hold_on_other_key_press(in[4] != 0);
            break;
        default:
            break;
    }
}

static void vial_qmk_settings_reset(void)
{
    dynamic_keymap_reset_settings();
}

// id_dynamic_entry_op: sub-op GET_NUMBER_OF_ENTRIES replies with zero entries.
static void vial_dynamic_entry_op(const uint8_t *in, uint8_t *out)
{
    if (in[2] != VIAL_DYNAMIC_ENTRY_GET_NUMBER_OF_ENTRIES) {
        return; // echo the request unchanged, like the reference
    }
    for (uint8_t i = 0; i < VIAL_REPORT_SIZE; i++) {
        out[i] = 0;
    }
}

static void vial_handle_prefix(const uint8_t *in, uint8_t *out)
{
    switch (in[1]) {
        case CMD_VIAL_GET_KEYBOARD_ID:
            vial_get_keyboard_id(out);
            return;

        case CMD_VIAL_GET_SIZE:
            vial_get_size(out);
            return;

        case CMD_VIAL_GET_DEFINITION:
            vial_get_definition(in, out);
            return;

        case CMD_VIAL_GET_UNLOCK_STATUS:
            vial_get_unlock_status(out);
            return;

        case CMD_VIAL_UNLOCK_START:
            vial_unlock_in_progress = 1;
            vial_unlocked           = 1;
            return;

        case CMD_VIAL_UNLOCK_POLL:
            vial_unlock_poll(out);
            return;

        case CMD_VIAL_LOCK:
            vial_unlocked           = 0;
            vial_unlock_in_progress = 0;
            return;

        case CMD_VIAL_QMK_SETTINGS_QUERY:
            vial_qmk_settings_query(out);
            return;

        case CMD_VIAL_QMK_SETTINGS_GET:
            vial_qmk_settings_get(in, out);
            return;

        case CMD_VIAL_QMK_SETTINGS_SET:
            vial_qmk_settings_set(in);
            return;

        case CMD_VIAL_QMK_SETTINGS_RESET:
            vial_qmk_settings_reset();
            return;

        case CMD_VIAL_DYNAMIC_ENTRY_OP:
            vial_dynamic_entry_op(in, out);
            return;

        default:
            return; // echo the request unchanged, like the reference
    }
}

// --- VIA commands (reply at out[1]) ---------------------------------------

// Flat keymap index -> layer/row/col, Vial's layer-major row-major order.
static bool keymap_pos(uint16_t idx, uint8_t *layer, uint8_t *row, uint8_t *col)
{
    if (idx >= (uint16_t)(VIAL_LAYERS * MATRIX_ROWS * MATRIX_COLS)) {
        return false;
    }
    *layer      = (uint8_t)(idx / (MATRIX_ROWS * MATRIX_COLS));
    uint8_t pos = (uint8_t)(idx % (MATRIX_ROWS * MATRIX_COLS));
    *row        = (uint8_t)(pos / MATRIX_COLS);
    *col        = (uint8_t)(pos % MATRIX_COLS);
    return true;
}

// Read the dynamic store. Empty matrix positions read as KC_NO.
static uint16_t vial_keymap_get(uint8_t layer, uint8_t row, uint8_t col)
{
    return dynamic_keymap_get(layer, row, col);
}

static void vial_get_matrix_state(uint8_t *out)
{
    uint8_t i = 2; // command id at [0], sub-command at [1], data from [2]
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        uint16_t bits = 0;
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            if (matrix_is_on(row, col)) {
                bits |= (uint16_t)1 << col;
            }
        }
        // Two bytes per row (14 cols), high byte first, like vial-qmk.
        out[i++] = (uint8_t)(bits >> 8);
        out[i++] = (uint8_t)bits;
    }
}

// id_dynamic_keymap_get_buffer: offset (BE) at in[1..2], size at in[3], reply
// keycodes at out[4..], each big-endian.
static void vial_keymap_get_buffer(const uint8_t *in, uint8_t *out)
{
    const uint16_t offset = rd16be(in + 1);
    uint8_t        size   = in[3]; // <= 28
    if (size > 28) {
        size = 28;
    }
    for (uint8_t i = 0; i < size; i++) {
        uint8_t  layer, row, col;
        uint16_t kc        = keymap_pos((uint16_t)(offset + i), &layer, &row, &col) ? vial_keymap_get(layer, row, col) : KC_NO;
        out[4 + i * 2]     = (uint8_t)(kc >> 8);
        out[4 + i * 2 + 1] = (uint8_t)kc;
    }
}

// id_dynamic_keymap_set_keycode: layer/row/col at in[1..3], keycode (BE) at in[4..5].
static void vial_set_keycode(const uint8_t *in)
{
    dynamic_keymap_set_keycode(in[1], in[2], in[3], rd16be(in + 4));
}

// id_dynamic_keymap_set_buffer: offset (BE) at in[1..2], size at in[3],
// keycodes at in[4..], each big-endian.
static void vial_keymap_set_buffer(const uint8_t *in)
{
    dynamic_keymap_set_offset(rd16be(in + 1), in + 4, in[3]);
}

static void vial_handle_via(const uint8_t *in, uint8_t *out)
{
    switch (in[0]) {
        case CMD_VIA_GET_PROTOCOL_VERSION:
            out[1] = (uint8_t)(VIA_PROTOCOL_VERSION >> 8);
            out[2] = (uint8_t)VIA_PROTOCOL_VERSION;
            return;

        case CMD_VIA_GET_KEYBOARD_VALUE:
            if (in[1] == VIA_UPTIME) {
                const uint32_t ms = tick_ms();
                out[2]            = (uint8_t)(ms & 0xFF);
                out[3]            = (uint8_t)((ms >> 8) & 0xFF);
                out[4]            = (uint8_t)((ms >> 16) & 0xFF);
                out[5]            = (uint8_t)((ms >> 24) & 0xFF);
            } else if (in[1] == VIA_LAYOUT_OPTIONS) {
                out[2] = 0;
            } else if (in[1] == VIA_SWITCH_MATRIX_STATE) {
                vial_get_matrix_state(out);
            }
            return;

        case CMD_VIA_SET_KEYBOARD_VALUE:
            // Only layout options are settable, and there is a single layout.
            return;

        case CMD_VIA_GET_KEYCODE: {
            uint16_t kc = vial_keymap_get(in[1], in[2], in[3]);
            wr16be(out + 4, kc);
            return;
        }

        case CMD_VIA_SET_KEYCODE:
            vial_set_keycode(in);
            return;

        case CMD_VIA_RESET_KEYCODE:
            dynamic_keymap_reset_keycode(in[1], in[2], in[3]);
            return;

        case CMD_VIA_MACRO_GET_COUNT:
            out[1] = 0;
            return;

        case CMD_VIA_MACRO_GET_BUFFER_SIZE:
            out[1] = 0;
            out[2] = 0;
            return;

        case CMD_VIA_GET_LAYER_COUNT:
            out[1] = VIAL_LAYERS;
            return;

        case CMD_VIA_KEYMAP_GET_BUFFER:
            vial_keymap_get_buffer(in, out);
            return;

        case CMD_VIA_KEYMAP_SET_BUFFER:
            vial_keymap_set_buffer(in);
            return;

        default:
            out[0] = VIA_UNHANDLED;
            return;
    }
}

void vial_handle(const uint8_t *in, uint8_t *out)
{
    // Seed the reply with the request: VIA commands echo the command id at
    // out[0], and unhandled Vial commands echo the whole request.
    for (uint8_t i = 0; i < VIAL_REPORT_SIZE; i++) {
        out[i] = in[i];
    }

    if (in[0] == CMD_VIA_VIAL_PREFIX) {
        vial_handle_prefix(in, out);
    } else {
        vial_handle_via(in, out);
    }
}

#endif // VIAL_ENABLE
