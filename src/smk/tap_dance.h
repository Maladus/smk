#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Tap dance engine.
//
// matrix.c feeds every resolved key event here first. A QK_TAP_DANCE key is
// held back until the tapping term decides it: a single tap emits on_tap, two
// quick taps emit on_double_tap, and a hold emits on_hold (or on_tap_hold after
// a preceding tap). Any other key pressed while a dance is undecided resolves
// it as a tap.
void tap_dance_init(void);

// Returns true when the event was consumed by the tap-dance engine.
bool tap_dance_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed);

// Checks the tapping term each main-loop pass.
void tap_dance_task(void);

#endif // VIAL_ENABLE
