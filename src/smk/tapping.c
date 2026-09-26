#include "tapping.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "report.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stdint.h>

// Events held while the tap/hold decision is pending. Enough for a fast roll
// plus the release of the LT key itself; older events are dropped if it fills.
#    define TAPPING_BUFFER 8

typedef struct {
    bool     undecided;   // an LT/TT/MT press is waiting for the term
    bool     hold_active; // decided hold; the layer/mod stays on until release
    bool     tap_toggle;  // tap toggles the layer (TT) instead of tapping a keycode
    bool     mod_tap;     // hold applies a modifier (MT) instead of a layer
    uint8_t  row;
    uint8_t  col;
    uint8_t  hold_layer;
    uint8_t  hold_mods;
    uint16_t tap_keycode;
    uint32_t start_ms;
    uint8_t  count;
    uint8_t  ev_row[TAPPING_BUFFER];
    uint8_t  ev_col[TAPPING_BUFFER];
    bool     ev_pressed[TAPPING_BUFFER];
} tapping_state_t;

static tapping_state_t tapping;

static void buffer_event(uint8_t row, uint8_t col, bool pressed)
{
    if (tapping.count >= TAPPING_BUFFER) {
        return;
    }
    tapping.ev_row[tapping.count]     = row;
    tapping.ev_col[tapping.count]     = col;
    tapping.ev_pressed[tapping.count] = pressed;
    tapping.count++;
}

// Replays buffered events through the normal path, so they resolve under the
// layer state the decision left behind.
static void replay(void)
{
    for (uint8_t i = 0; i < tapping.count; i++) {
        matrix_process_key(tapping.ev_row[i], tapping.ev_col[i], tapping.ev_pressed[i]);
    }
    tapping.count = 0;
}

static void decide_hold(void)
{
    tapping.undecided   = false;
    tapping.hold_active = true;
    if (tapping.mod_tap) {
        add_mods(MODS_5BIT_TO_8BIT(tapping.hold_mods));
        send_keyboard_report();
    } else {
        matrix_layer_activate(tapping.hold_layer);
    }
    replay();
}

static void decide_tap(void)
{
    tapping.undecided = false;
    if (tapping.tap_toggle) {
        matrix_layer_toggle(tapping.hold_layer);
    } else {
        matrix_tap_keycode(tapping.tap_keycode);
    }
    replay();
}

// Arm the tap/hold decision for an LT (tap_keycode), TT (tap_toggle) or MT
// (mod_tap) key.
static void arm(uint8_t row, uint8_t col, uint8_t layer, uint8_t mods, uint16_t tap_keycode, bool tap_toggle, bool mod_tap)
{
    if (layer >= VIAL_LAYERS) {
        layer = (uint8_t)(VIAL_LAYERS - 1);
    }
    tapping.undecided   = true;
    tapping.hold_active = false;
    tapping.tap_toggle  = tap_toggle;
    tapping.mod_tap     = mod_tap;
    tapping.row         = row;
    tapping.col         = col;
    tapping.hold_layer  = layer;
    tapping.hold_mods   = mods;
    tapping.tap_keycode = tap_keycode;
    tapping.start_ms    = tick_ms();
    tapping.count       = 0;
}

bool tapping_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    if (tapping.undecided) {
        if (row == tapping.row && col == tapping.col) {
            if (!pressed) {
                if ((tick_ms() - tapping.start_ms) < dynamic_keymap_tapping_term()) {
                    decide_tap();
                } else {
                    decide_hold();
                }
            }
            return true;
        }

        // Another key decides the hold early under the configured flags; process
        // that event right away so it lands on the freshly activated layer.
        if (pressed && dynamic_keymap_hold_on_other_key_press()) {
            decide_hold();
            return false;
        }
        if (!pressed && dynamic_keymap_permissive_hold()) {
            decide_hold();
            return false;
        }

        buffer_event(row, col, pressed);
        return true;
    }

    if (tapping.hold_active && row == tapping.row && col == tapping.col && !pressed) {
        tapping.hold_active = false;
        if (tapping.mod_tap) {
            del_mods(MODS_5BIT_TO_8BIT(tapping.hold_mods));
            send_keyboard_report();
        } else {
            matrix_layer_deactivate(tapping.hold_layer);
        }
        return true;
    }

    if (pressed && IS_QK_MOD_TAP(keycode)) {
        arm(row, col, 0, QK_MOD_TAP_GET_MODS(keycode), QK_MOD_TAP_GET_TAP_KEYCODE(keycode), false, true);
        return true;
    }

    if (pressed && IS_QK_LAYER_TAP(keycode)) {
        arm(row, col, QK_LAYER_TAP_GET_LAYER(keycode), 0, QK_LAYER_TAP_GET_TAP_KEYCODE(keycode), false, false);
        return true;
    }

    if (pressed && IS_QK_LAYER_TAP_TOGGLE(keycode)) {
        arm(row, col, QK_LAYER_TAP_TOGGLE_GET_LAYER(keycode), 0, 0, true, false);
        return true;
    }

    return false;
}

void tapping_task(void)
{
    if (tapping.undecided && (tick_ms() - tapping.start_ms) >= dynamic_keymap_tapping_term()) {
        decide_hold();
    }
}

#endif // VIAL_ENABLE
