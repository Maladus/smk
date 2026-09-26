#include "tap_dance.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "matrix.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stdint.h>

#    if VIAL_TAP_DANCE_ENTRIES > 0

typedef struct {
    bool     active;
    bool     pressed;
    bool     hold_active;
    uint8_t  index;
    uint8_t  count;
    uint8_t  row;
    uint8_t  col;
    uint16_t hold_kc;
    uint32_t start_ms;
} tap_dance_state_t;

static tap_dance_state_t td;

static uint16_t td_term(uint8_t index)
{
    vial_tap_dance_entry_t e;
    if (dynamic_keymap_get_tap_dance(index, &e) == 0 && e.custom_tapping_term != 0) {
        return e.custom_tapping_term;
    }
    return dynamic_keymap_tapping_term();
}

static void td_reset(void)
{
    td.active      = false;
    td.pressed     = false;
    td.hold_active = false;
    td.index       = 0;
    td.count       = 0;
    td.hold_kc     = 0;
}

static void td_start(uint8_t row, uint8_t col, uint8_t index)
{
    td.active      = true;
    td.pressed     = true;
    td.hold_active = false;
    td.index       = index;
    td.count       = 1;
    td.row         = row;
    td.col         = col;
    td.hold_kc     = 0;
    td.start_ms    = tick_ms();
}

// Emit the tap action for the accumulated count and clear the dance.
static void td_decide_tap(void)
{
    vial_tap_dance_entry_t e;
    if (dynamic_keymap_get_tap_dance(td.index, &e) == 0) {
        uint16_t kc = e.on_tap;
        if (td.count == 2 && e.on_double_tap != 0) {
            kc = e.on_double_tap;
        }
        if (kc != 0) {
            matrix_tap_keycode(kc);
        }
    }
    td_reset();
}

// Emit the hold action (press) and keep it active until the key is released.
static void td_decide_hold(void)
{
    vial_tap_dance_entry_t e;
    if (dynamic_keymap_get_tap_dance(td.index, &e) == 0) {
        uint16_t kc = e.on_hold;
        if (td.count >= 2 && e.on_tap_hold != 0) {
            kc = e.on_tap_hold;
        }
        if (kc == 0) {
            kc = e.on_tap;
        }
        td.hold_kc = kc;
    }
    td.hold_active = true;
    if (td.hold_kc != 0) {
        matrix_send_keycode(td.hold_kc, true);
    }
}

void tap_dance_init(void)
{
    td_reset();
}

bool tap_dance_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    if (IS_QK_TAP_DANCE(keycode)) {
        const uint8_t index = (uint8_t)QK_TAP_DANCE_GET_INDEX(keycode);
        if (pressed) {
            if (td.active && td.row == row && td.col == col) {
                td.count++;
                td.pressed     = true;
                td.hold_active = false;
                td.start_ms    = tick_ms();
            } else {
                if (td.active) {
                    td_decide_tap();
                }
                td_start(row, col, index);
            }
        } else if (td.active && td.row == row && td.col == col) {
            td.pressed = false;
            if (td.hold_active) {
                if (td.hold_kc != 0) {
                    matrix_send_keycode(td.hold_kc, false);
                }
                td_reset();
            } else if ((tick_ms() - td.start_ms) >= td_term(td.index)) {
                // The term elapsed before the task ran: a short hold.
                td_decide_hold();
                if (td.hold_kc != 0) {
                    matrix_send_keycode(td.hold_kc, false);
                }
                td_reset();
            }
        }
        return true;
    }

    // A non-tap-dance key decides an undecided dance as a tap.
    if (td.active) {
        td_decide_tap();
    }
    return false;
}

void tap_dance_task(void)
{
    if (!td.active || td.hold_active) {
        return;
    }
    if ((tick_ms() - td.start_ms) >= td_term(td.index)) {
        if (td.pressed) {
            td_decide_hold();
        } else {
            td_decide_tap();
        }
    }
}

#    else

void tap_dance_init(void) {}

bool tap_dance_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    (void)row;
    (void)col;
    (void)keycode;
    (void)pressed;
    return false;
}

void tap_dance_task(void) {}

#    endif
#endif // VIAL_ENABLE
