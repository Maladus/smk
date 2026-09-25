#pragma once

#include <stdint.h>

// The keyboard's realtime work, driven by the platform's periodic tick.
//
// One hardware timer has to serve two jobs that can't overlap: sweeping the key
// matrix, and emitting LED PWM subframes (LED drive current couples into the row
// sense). So ticks alternate between them, and this is where that interleave is
// decided.

void tick_init(void);

void tick_dispatch(void);

void tick_pause(void);
void tick_resume(void);

#ifdef VIAL_ENABLE
// Milliseconds since boot, for the tapping engine. Counted from the timer2
// periods (100 us scan, 400 us LED subframe) in the tick interrupt.
uint32_t tick_ms(void);
#endif
