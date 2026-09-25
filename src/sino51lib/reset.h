#pragma once

#include <stdbool.h>
#include <stdint.h>

// What brought the part up, as RSTSTAT read it.
extern uint8_t reset_status;

// RSTSTAT doubles as the watchdog kick, so the reset-source flags survive only until
// something kicks. This has to run before anything else on the platform.
void reset_init(void);

// True when the last reset was the watchdog. Read after reset_init() and before
// the first watchdog kick; RSTSTAT clears the flags on every kick.
bool reset_was_watchdog(void);
