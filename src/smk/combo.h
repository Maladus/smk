#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Combo engine.
//
// matrix.c feeds every resolved key event here before the normal path. A combo
// fires when all of its input keycodes are held within the combo term: the
// source keys are suppressed and the output keycode is emitted instead. The
// output is released when any source key is released.
void combo_init(void);

// Returns true when the event was consumed by the combo engine (buffered or
// suppressed). Keys that are not part of any combo pass straight through.
bool combo_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed);

// Flushes buffered presses once the combo term has elapsed.
void combo_task(void);

#endif // VIAL_ENABLE
