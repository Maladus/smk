#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIAL_ENABLE

// VIA macro player.
//
// A QK_MACRO key starts playback of the corresponding macro in the buffer: the
// player walks the send-string encoded bytes, emitting taps/downs/ups and
// pausing on delays. macro_task() advances playback each main-loop pass.
void macro_init(void);

// Returns true when the keycode is a macro trigger (and consumes it).
bool macro_process_record(uint16_t keycode, bool pressed);

// Advances playback; call each main-loop pass.
void macro_task(void);

#endif // VIAL_ENABLE
