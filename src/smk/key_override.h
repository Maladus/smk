#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// Key override engine.
//
// matrix.c calls this for every key that reaches the normal path. A key whose
// keycode, layer and modifier state match an enabled override entry emits the
// entry's replacement instead; the replacement is released when the trigger is
// released.
void key_override_init(void);

// Returns true when the event was replaced. `layer` is the layer the event
// resolved on.
bool key_override_process_record(uint16_t keycode, uint8_t layer, bool pressed);

#endif // VIAL_ENABLE
