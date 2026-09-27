#pragma once

// RC-timing battery measurement on P0.0/P0.1 (the SH68F90 has no ADC). Run it
// at boot and on wake; it writes keyboard_state.battery_level (0..7) and
// keyboard_state.low_power.
void user_battery_measure(void);
