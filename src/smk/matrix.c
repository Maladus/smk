#include "matrix.h"
#include "report.h"
#include "debug.h"
#include "layout.h"
#include "user_layout.h"
#include "kb.h"
#include "user_matrix.h"
#include "kbdef.h"
#include "host.h"
#include "delay.h"
#include "indicators.h"
#include "sleep.h"
#ifdef ISP_ENABLE
#    include "isp.h"
#endif
#include <stdlib.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE
#    include "combo.h"
#    include "dynamic_keymap.h"
#    include "key_override.h"
#    include "tap_dance.h"
#    include "tapping.h"
#endif

typedef uint8_t matrix_col_t;

matrix_col_t matrix[MATRIX_COLS];
matrix_col_t matrix_previous[MATRIX_COLS];

volatile bool matrix_updated;

uint8_t action_layer;

uint8_t default_layer;

// Vial reads keys through the dynamic store; without it the const keymap is
// used directly, so the generated code is unchanged.
#ifdef VIAL_ENABLE
#    define KEYMAP_GET(layer, row, col) dynamic_keymap_get((layer), (row), (col))
#else
#    define KEYMAP_GET(layer, row, col) (keymaps[(layer)][(row)][(col)])
#endif

#ifdef VIAL_ENABLE
// Layer engine state. `layer_state` holds the momentary/toggled layers; the
// default layer is always active and is the fallthrough base. `press_layer`
// records the layer each held key resolved under, so a release uses the same
// keycode it pressed even after the layer state changed.
#    if VIAL_LAYERS <= 4
#        define VIAL_PRESS_BITS 2
#    else
#        define VIAL_PRESS_BITS 4
#    endif
#    define VIAL_PRESS_BYTES (((MATRIX_ROWS * MATRIX_COLS) * VIAL_PRESS_BITS + 7) / 8)

static uint16_t layer_state;
static uint8_t  press_layer[VIAL_PRESS_BYTES];

// One-shot layer (OSL): active from its press until the next key is released.
static uint8_t osl_layer = 0xFF;
static uint8_t osl_used;
static uint8_t osl_row;
static uint8_t osl_col;

// One-shot mod (OSM): applies the modifier until the next key is released.
static uint8_t osm_mods;
static uint8_t osm_used;
static uint8_t osm_row;
static uint8_t osm_col;
#endif

// Recovery check for the boot path: true when the top-left key (R0/C0) is held
// at power-on. It drives the column directly instead of going through the scan
// ISR, so it works before matrix_init() and before USB, which is exactly when a
// wedged image cannot answer the host's ISP feature report or reach Fn+B.
bool matrix_recovery_held(void)
{
    user_matrix_cols_deselect_all();
    user_matrix_col_select(0);
    delay_us(50);                                            // let the row line settle before sampling
    const bool held = (user_matrix_read_rows() & 0x01) == 0; // R0 is bit 0, active-low
    user_matrix_col_deselect(0);
    user_matrix_cols_deselect_all();
    return held;
}

void matrix_init()
{
    action_layer   = 0;
    default_layer  = 0;
    matrix_updated = false;

    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix[i]          = 0;
        matrix_previous[i] = 0;
    }

#ifdef VIAL_ENABLE
    layer_state = 0;
    osl_layer   = 0xFF;
    osl_used    = 0;
    osm_mods    = 0;
    osm_used    = 0;
    for (uint8_t i = 0; i < VIAL_PRESS_BYTES; i++) {
        press_layer[i] = 0;
    }
    dynamic_keymap_init();
    combo_init();
    tap_dance_init();
    key_override_init();
#endif
}

void set_default_layer(uint8_t layer)
{
    default_layer = layer;
}

static uint16_t resolve_keycode(uint16_t base, uint8_t row, uint8_t col)
{
    if (!action_layer) {
        return base;
    }

    const uint16_t overlay = KEYMAP_GET(action_layer, row, col);
    return (overlay == KC_TRANSPARENT) ? base : overlay;
}

static void send_keycode(uint16_t qcode, bool pressed)
{
    if (IS_MODIFIER_KEYCODE(qcode)) {
        if (pressed) {
            add_mods(MOD_BIT((uint8_t)(qcode & 0xFF)));
        } else {
            del_mods(MOD_BIT((uint8_t)(qcode & 0xFF)));
        }
        send_keyboard_report();
        return;
    }

    if (IS_BASIC_KEYCODE(qcode)) {
        if (pressed) {
            add_key((uint8_t)(qcode & 0xFF));
        } else {
            del_key((uint8_t)(qcode & 0xFF));
        }
        send_keyboard_report();
        return;
    }

    if (IS_SYSTEM_KEYCODE(qcode)) {
        host_system_send(pressed ? keycode_to_system(qcode) : 0);
        return;
    }

    if (IS_CONSUMER_KEYCODE(qcode)) {
        host_consumer_send(pressed ? keycode_to_consumer(qcode) : 0);
        return;
    }
}

#ifdef VIAL_ENABLE
static uint8_t press_layer_get(uint8_t row, uint8_t col)
{
    const uint16_t bit   = (uint16_t)((uint16_t)row * MATRIX_COLS + col) * VIAL_PRESS_BITS;
    const uint8_t  byte  = (uint8_t)(bit >> 3);
    const uint8_t  shift = (uint8_t)(bit & 7);
    return (uint8_t)((press_layer[byte] >> shift) & ((1u << VIAL_PRESS_BITS) - 1u));
}

static void press_layer_set(uint8_t row, uint8_t col, uint8_t layer)
{
    const uint16_t bit   = (uint16_t)((uint16_t)row * MATRIX_COLS + col) * VIAL_PRESS_BITS;
    const uint8_t  byte  = (uint8_t)(bit >> 3);
    const uint8_t  shift = (uint8_t)(bit & 7);
    const uint8_t  mask  = (uint8_t)(((1u << VIAL_PRESS_BITS) - 1u) << shift);
    press_layer[byte]    = (uint8_t)((press_layer[byte] & ~mask) | ((layer << shift) & mask));
}

static uint8_t clamp_layer(uint8_t layer)
{
    return (layer < VIAL_LAYERS) ? layer : (uint8_t)(VIAL_LAYERS - 1);
}

// Active layers as a bitmask: the momentary/toggled layers plus the default.
// Fn+Shift also raises the secondary Fn layer, whose number row carries the
// media keys the manual documents as Fn+F1..F12.
static uint16_t active_layers(void)
{
    uint16_t state = (uint16_t)(layer_state | (uint16_t)(1u << default_layer));
    if ((state & (uint16_t)(1u << LAYER_FN)) && (get_mods() & MODS_SHIFT_MASK)) {
        state |= (uint16_t)(1u << LAYER_FN_SHIFT);
    }
    return state;
}

// Highest active layer with a non-transparent keycode, falling through
// transparent entries to the default layer.
static uint8_t resolve_layer(uint8_t row, uint8_t col)
{
    const uint16_t state = active_layers();
    for (int8_t l = VIAL_LAYERS - 1; l >= 0; l--) {
        if (!(state & (uint16_t)(1u << l))) {
            continue;
        }
        if (KEYMAP_GET((uint8_t)l, row, col) != KC_TRANSPARENT) {
            return (uint8_t)l;
        }
    }
    return default_layer;
}

void matrix_layer_activate(uint8_t layer)
{
    layer_state |= (uint16_t)(1u << clamp_layer(layer));
}

void matrix_layer_deactivate(uint8_t layer)
{
    layer_state &= (uint16_t)~(uint16_t)(1u << clamp_layer(layer));
}

void matrix_layer_toggle(uint8_t layer)
{
    layer_state ^= (uint16_t)(1u << clamp_layer(layer));
}

static void osl_activate(uint8_t layer)
{
    if (osl_layer != 0xFF) {
        matrix_layer_deactivate(osl_layer);
    }
    osl_layer = clamp_layer(layer);
    osl_used  = 0;
    matrix_layer_activate(osl_layer);
}

// Consume the one-shot layer on the first key press, then clear it when that
// key is released. The OSL key itself is handled by handle_layer_keycode, so it
// never reaches here.
static void osl_update(uint8_t row, uint8_t col, bool pressed)
{
    if (osl_layer == 0xFF) {
        return;
    }
    if (!osl_used) {
        if (pressed) {
            osl_used = 1;
            osl_row  = row;
            osl_col  = col;
        }
        return;
    }
    if (!pressed && row == osl_row && col == osl_col) {
        matrix_layer_deactivate(osl_layer);
        osl_layer = 0xFF;
        osl_used  = 0;
    }
}

static void osm_activate(uint8_t mods)
{
    if (osm_mods) {
        del_mods(MODS_5BIT_TO_8BIT(osm_mods));
    }
    osm_mods = mods;
    osm_used = 0;
    add_mods(MODS_5BIT_TO_8BIT(osm_mods));
    send_keyboard_report();
}

// Consume the one-shot mod on the first key press, then clear it when that key
// is released. The OSM key itself is handled by handle_layer_keycode.
static void osm_update(uint8_t row, uint8_t col, bool pressed)
{
    if (!osm_mods) {
        return;
    }
    if (!osm_used) {
        if (pressed) {
            osm_used = 1;
            osm_row  = row;
            osm_col  = col;
        }
        return;
    }
    if (!pressed && row == osm_row && col == osm_col) {
        del_mods(MODS_5BIT_TO_8BIT(osm_mods));
        osm_mods = 0;
        osm_used = 0;
        send_keyboard_report();
    }
}

// Layer keycodes act on the layer state and never reach the host.
static bool handle_layer_keycode(uint16_t kc, bool pressed)
{
    if (IS_QK_MOMENTARY(kc)) {
        if (pressed) {
            matrix_layer_activate(QK_MOMENTARY_GET_LAYER(kc));
        } else {
            matrix_layer_deactivate(QK_MOMENTARY_GET_LAYER(kc));
        }
        return true;
    }

    if (IS_QK_TOGGLE_LAYER(kc)) {
        if (pressed) {
            matrix_layer_toggle(QK_TOGGLE_LAYER_GET_LAYER(kc));
        }
        return true;
    }

    if (IS_QK_TO(kc)) {
        if (pressed) {
            layer_state = (uint16_t)(1u << clamp_layer(QK_TO_GET_LAYER(kc)));
        }
        return true;
    }

    if (IS_QK_DEF_LAYER(kc)) {
        if (pressed) {
            default_layer = clamp_layer(QK_DEF_LAYER_GET_LAYER(kc));
        }
        return true;
    }

    if (IS_QK_PERSISTENT_DEF_LAYER(kc)) {
        if (pressed) {
            const uint8_t layer = clamp_layer(QK_PERSISTENT_DEF_LAYER_GET_LAYER(kc));
            if (layer != default_layer) {
                default_layer = layer;
                dynamic_keymap_save_base_layer(layer);
            }
        }
        return true;
    }

    if (IS_QK_LAYER_MOD(kc)) {
        if (pressed) {
            matrix_layer_activate(QK_LAYER_MOD_GET_LAYER(kc));
            add_mods((uint8_t)QK_LAYER_MOD_GET_MODS(kc));
        } else {
            matrix_layer_deactivate(QK_LAYER_MOD_GET_LAYER(kc));
            del_mods((uint8_t)QK_LAYER_MOD_GET_MODS(kc));
        }
        send_keyboard_report();
        return true;
    }

    if (IS_QK_ONE_SHOT_LAYER(kc)) {
        if (pressed) {
            osl_activate(QK_ONE_SHOT_LAYER_GET_LAYER(kc));
        }
        return true;
    }

    if (IS_QK_ONE_SHOT_MOD(kc)) {
        if (pressed) {
            osm_activate(QK_ONE_SHOT_MOD_GET_MODS(kc));
        }
        return true;
    }

    return false;
}

static void dispatch_keycode(uint16_t qcode, bool pressed)
{
    if (!kb_process_record(qcode, pressed)) {
        return;
    }

    if (!layout_process_record(qcode, pressed)) {
        return;
    }

    send_keycode(qcode, pressed);
}

// Normal path for one key event: resolve the layer, run the layer keycodes and
// send what is left. The tapping engine replays buffered events through here,
// so it never re-enters the tap/hold decision.
void matrix_process_key(uint8_t row, uint8_t col, bool pressed)
{
    uint8_t  layer;
    uint16_t kc;

    if (pressed) {
        layer = resolve_layer(row, col);
        press_layer_set(row, col, layer);
        kc = KEYMAP_GET(layer, row, col);
    } else {
        layer = press_layer_get(row, col);
        kc    = KEYMAP_GET(layer, row, col);
        if (kc == KC_TRANSPARENT) {
            kc = KEYMAP_GET(default_layer, row, col);
        }
    }

    if (handle_layer_keycode(kc, pressed)) {
        return;
    }

#    ifdef ISP_ENABLE
    // QK_BOOTLOADER hands over to the ISP bootloader, the same path the host's
    // feature report takes. It is the on-keyboard recovery route when the host
    // cannot get the board into ISP mode.
    if (pressed && kc == QK_BOOTLOADER) {
        isp_jump();
    }
#    endif

    osl_update(row, col, pressed);
    osm_update(row, col, pressed);

    if (key_override_process_record(kc, layer, pressed)) {
        return;
    }

    dispatch_keycode(kc, pressed);
}

// Press and release a raw keycode; used for the tap half of a layer-tap.
void matrix_tap_keycode(uint16_t keycode)
{
    dispatch_keycode(keycode, true);
    dispatch_keycode(keycode, false);
}

// Send a raw keycode without resolving a matrix position; used by the combo
// engine to emit a combo output on press/release.
void matrix_send_keycode(uint16_t keycode, bool pressed)
{
    dispatch_keycode(keycode, pressed);
}
#endif // VIAL_ENABLE

static void process_key_state(uint8_t row, uint8_t col, bool pressed)
{
#ifdef VIAL_ENABLE
    const uint8_t  layer = pressed ? resolve_layer(row, col) : press_layer_get(row, col);
    const uint16_t kc    = KEYMAP_GET(layer, row, col);

    if (tap_dance_process_record(row, col, kc, pressed)) {
        return;
    }

    if (combo_process_record(row, col, kc, pressed)) {
        return;
    }

    if (tapping_process_record(row, col, kc, pressed)) {
        return;
    }

    matrix_process_key(row, col, pressed);
#else
    const uint16_t base = KEYMAP_GET(default_layer, row, col);

    if (IS_QK_MOMENTARY(base)) {
        if (pressed) {
            action_layer = QK_MOMENTARY_GET_LAYER(base);
        } else {
            clear_keys();
            action_layer = 0;
        }
        return;
    }

    const uint16_t qcode = resolve_keycode(base, row, col);

    if (!kb_process_record(qcode, pressed)) {
        return;
    }

    if (!layout_process_record(qcode, pressed)) {
        return;
    }

    send_keycode(qcode, pressed);
#endif
}

void matrix_scan_full(void)
{
    indicators_pwm_disable();

    user_matrix_sinks_off();

    user_matrix_scan_pre();
    user_matrix_cols_deselect_all();

    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        user_matrix_col_select(col);

        delay_us(1); // settle (was 10us; the 2x10us x 14-col sweep blocked USB ~320us)
        const uint8_t sample1 = user_matrix_read_rows();
        delay_us(1);
        const uint8_t sample2 = user_matrix_read_rows();

        if (sample1 == sample2) {
            matrix[col] = ~sample1;
        }

        user_matrix_col_deselect(col);
    }

    user_matrix_scan_post();

    indicators_pwm_enable();

    matrix_updated = true;
}

uint8_t matrix_task()
{
    if (!matrix_updated) {
        return false;
    }
    // Snapshot the scan-written matrix[], then diff it against
    // matrix_previous[]. No lock needed: each column byte reads atomically, so a
    // concurrent scan lands cleanly on one side of the read - at worst a
    // transition is split across two main-loop iterations, never lost.
    matrix_col_t snapshot[MATRIX_COLS];
    matrix_updated = false;
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        snapshot[i] = matrix[i];
    }

    bool matrix_changed = false;

    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        const matrix_col_t current_col = snapshot[col];
        const matrix_col_t col_changes = current_col ^ matrix_previous[col];
        if (!col_changes) {
            continue;
        }
        matrix_changed = true;
        sleep_note_activity(); // a key changed state; reset the inactivity timer

        matrix_col_t row_mask = 1;
        for (uint8_t row = 0; row < MATRIX_ROWS; row++, row_mask <<= 1) {
            if (col_changes & row_mask) {
                const bool key_pressed = current_col & row_mask;
                process_key_state(row, col, key_pressed);
            }
        }

        matrix_previous[col] = current_col;
    }

    return matrix_changed;
}

#ifdef VIAL_ENABLE
bool matrix_is_on(uint8_t row, uint8_t col)
{
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        return false;
    }
    return (matrix[col] >> row) & 1;
}
#endif
