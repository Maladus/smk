#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Layer-tap (LT) decision engine.
//
// matrix.c feeds every key event here before the normal path. A press of an LT
// key is held back until the tapping term decides it: a tap replays as the tap
// keycode, a hold activates the layer. Other key events that arrive while the
// decision is pending are buffered and replayed afterwards, so nothing is lost.
bool tapping_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed);

// Checks the tapping term each main-loop pass.
void tapping_task(void);

#endif // VIAL_ENABLE
