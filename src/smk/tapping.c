#include "tapping.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stdint.h>

// Events held while the tap/hold decision is pending. Enough for a fast roll
// plus the release of the LT key itself; older events are dropped if it fills.
#    define TAPPING_BUFFER 8

typedef struct {
    bool     undecided;   // an LT press is waiting for the term
    bool     hold_active; // decided hold; the layer stays on until release
    uint8_t  row;
    uint8_t  col;
    uint8_t  hold_layer;
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
    matrix_layer_activate(tapping.hold_layer);
    replay();
}

static void decide_tap(void)
{
    tapping.undecided = false;
    matrix_tap_keycode(tapping.tap_keycode);
    replay();
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
        matrix_layer_deactivate(tapping.hold_layer);
        return true;
    }

    if (pressed && IS_QK_LAYER_TAP(keycode)) {
        uint8_t layer = QK_LAYER_TAP_GET_LAYER(keycode);
        if (layer >= VIAL_LAYERS) {
            layer = (uint8_t)(VIAL_LAYERS - 1);
        }
        tapping.undecided   = true;
        tapping.hold_active = false;
        tapping.row         = row;
        tapping.col         = col;
        tapping.hold_layer  = layer;
        tapping.tap_keycode = QK_LAYER_TAP_GET_TAP_KEYCODE(keycode);
        tapping.start_ms    = tick_ms();
        tapping.count       = 0;
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
