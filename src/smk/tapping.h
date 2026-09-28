#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Layer-tap (LT), layer-tap-toggle (TT) and mod-tap (MT) decision engine.
//
// matrix.c feeds every key event here before the normal path. A press of a
// tap/hold key is held back until the tapping term decides it: a tap replays as
// the tap keycode, a hold activates the layer or applies the modifier. Each key
// that is down owns its own slot, so releasing one key only clears the layer or
// modifier its own press set - rolling two home-row mods, or a re-press of the
// same key, cannot leak or steal the other key's hold. Other key events that
// arrive while a decision is pending are buffered and replayed afterwards, back
// through the tap/hold decision, so nothing is lost.
bool tapping_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed);

// Checks the tapping term each main-loop pass.
void tapping_task(void);

#endif // VIAL_ENABLE
