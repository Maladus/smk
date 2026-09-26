#include "combo.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "matrix.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stddef.h>
#    include <stdint.h>

#    if VIAL_COMBO_ENTRIES > 0

// How long the source keys may be spread apart and still count as a chord.
#        define COMBO_TERM 50

// Buffered source presses; enough for a few overlapping chords.
#        define COMBO_BUFFER 16

typedef struct {
    uint8_t  row;
    uint8_t  col;
    uint16_t kc;
} combo_press_t;

static combo_press_t press_buf[COMBO_BUFFER];
static uint8_t       press_count;
static uint32_t      window_start;
static bool          window_active;
static uint8_t       active_combo; // 0xFF when none

static bool combo_contains(const vial_combo_entry_t *e, uint16_t kc)
{
    for (uint8_t i = 0; i < 4; i++) {
        if (e->input[i] != KC_NO && e->input[i] == kc) {
            return true;
        }
    }
    return false;
}

static bool combo_key_part(uint16_t kc)
{
    if (kc == KC_NO || kc == KC_TRANSPARENT) {
        return false;
    }
    const uint8_t n = dynamic_keymap_combo_count();
    for (uint8_t i = 0; i < n; i++) {
        vial_combo_entry_t e;
        if (dynamic_keymap_get_combo(i, &e) == 0 && combo_contains(&e, kc)) {
            return true;
        }
    }
    return false;
}

static bool combo_is_held(const vial_combo_entry_t *e)
{
    uint8_t needed = 0;
    for (uint8_t k = 0; k < 4; k++) {
        if (e->input[k] == KC_NO) {
            continue;
        }
        needed++;
        bool found = false;
        for (uint8_t i = 0; i < press_count; i++) {
            if (press_buf[i].kc == e->input[k]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return needed >= 2;
}

static uint8_t combo_find(void)
{
    const uint8_t n = dynamic_keymap_combo_count();
    for (uint8_t i = 0; i < n; i++) {
        vial_combo_entry_t e;
        if (dynamic_keymap_get_combo(i, &e) == 0 && combo_is_held(&e)) {
            return i;
        }
    }
    return 0xFF;
}

static void combo_add_press(uint8_t row, uint8_t col, uint16_t kc)
{
    if (press_count >= COMBO_BUFFER) {
        return;
    }
    press_buf[press_count].row = row;
    press_buf[press_count].col = col;
    press_buf[press_count].kc  = kc;
    press_count++;
}

static void combo_remove_press(uint8_t row, uint8_t col)
{
    for (uint8_t i = 0; i < press_count; i++) {
        if (press_buf[i].row == row && press_buf[i].col == col) {
            for (uint8_t j = (uint8_t)(i + 1); j < press_count; j++) {
                press_buf[j - 1] = press_buf[j];
            }
            press_count--;
            return;
        }
    }
}

// Dispatch the buffered presses that are not part of `except` (a combo whose
// keys are suppressed).
static void combo_flush_except(const vial_combo_entry_t *except)
{
    for (uint8_t i = 0; i < press_count; i++) {
        if (except && combo_contains(except, press_buf[i].kc)) {
            continue;
        }
        matrix_process_key(press_buf[i].row, press_buf[i].col, true);
    }
    press_count = 0;
}

void combo_init(void)
{
    press_count   = 0;
    window_active = false;
    active_combo  = 0xFF;
}

bool combo_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    if (pressed) {
        if (!combo_key_part(keycode)) {
            // A non-combo key decides any pending chord: flush the buffered
            // source presses so they type, then let this key through.
            if (press_count > 0) {
                combo_flush_except(NULL);
                window_active = false;
            }
            return false;
        }
        if (!window_active) {
            window_active = true;
            press_count   = 0;
        }
        window_start = tick_ms();
        combo_add_press(row, col, keycode);

        const uint8_t idx = combo_find();
        if (idx != 0xFF) {
            vial_combo_entry_t e;
            dynamic_keymap_get_combo(idx, &e);
            combo_flush_except(&e);
            matrix_send_keycode(e.output, true);
            active_combo  = idx;
            window_active = false;
            return true;
        }
        return true;
    }

    if (active_combo != 0xFF) {
        vial_combo_entry_t e;
        if (dynamic_keymap_get_combo(active_combo, &e) == 0 && combo_contains(&e, keycode)) {
            matrix_send_keycode(e.output, false);
            active_combo = 0xFF;
            combo_remove_press(row, col);
            return true;
        }
    }

    if (press_count > 0) {
        combo_flush_except(NULL);
        window_active = false;
    }
    combo_remove_press(row, col);
    return false;
}

void combo_task(void)
{
    if (window_active && (tick_ms() - window_start) >= COMBO_TERM) {
        combo_flush_except(NULL);
        window_active = false;
    }
}

#    else

void combo_init(void) {}

bool combo_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    (void)row;
    (void)col;
    (void)keycode;
    (void)pressed;
    return false;
}

void combo_task(void) {}

#    endif
#endif // VIAL_ENABLE
