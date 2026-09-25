#pragma once

// RC-timing battery measurement on P0.0/P0.1 (the SH68F90 has no ADC). Run it
// at boot and on wake; it writes keyboard_state.battery_level (0..7),
// keyboard_state.battery_level_rc, and keyboard_state.low_power.
void user_battery_measure(void);

// Debug helper: every BATTERY_DIAG_INTERVAL_TICKS main-loop passes, re-measure
// the RC level and log it next to the RF module's own level. Compiled to a
// no-op unless DEBUG is set. Call it from the main loop.
void user_battery_diag_task(void);
