#include "kbdef.h"
#include "user_battery.h"
#include "keyboard.h"
#include "gpio.h"
#include "watchdog.h"
#include "delay.h"
#include "debug.h"

// RC battery measurement, reversed from the stock firmware's boot/wake routine
// (0xF770, called from the 0xF000 boot entry). The SH68F90 has no ADC, so the
// battery voltage is read with a bit-banged RC-timing loop on P0.0/P0.1:
//
//   P0.1 = charge/discharge pin. Driven low it discharges the capacitor;
//          released with its pull-up enabled it charges the capacitor.
//   P0.0 = sense pin. It reads the capacitor; the loop counts how long it
//          stays high after charging.
//
// The sequence below is the stock's measure loop "A" (0xF798): discharge for
// one delay unit, charge for ten delay units, then count (kicking the watchdog
// each iteration) while the sense pin stays high. The capacitor discharges
// through the battery path, so a higher battery holds the pin high longer --
// the count grows with battery voltage.
//
// Clock: the measure runs after clock_init(), so FREQ_SYS = 24 MHz on the 1T
// core (one machine cycle == one oscillator cycle). The stock's delay unit
// (0xFD28) is ~15 cycles, so one unit is ~0.6 us; delay_us() is used with the
// same 1:10 discharge:charge ratio. The exact RC time constant is board
// dependent, so the thresholds below are a first-order calibration that a
// hardware pass should refine.

#define BATTERY_DISCHARGE_US 1
#define BATTERY_CHARGE_US    10

// Stock count window: 0x0E = 0x64 (100).
#define BATTERY_MAX_COUNT 100

// 8 levels across the 100-count window. Linear is a first approximation; the
// RC discharge is logarithmic in the battery voltage.
#define BATTERY_LEVELS 8

void user_battery_measure(void)
{
    uint8_t count = 0;

    // Discharge the capacitor through P0.1 (P0.0's pull-up holds the sense pin
    // high while the output pin sinks the capacitor).
    GPIO_PULLUP_OFF(0, (uint8_t)(BAT_RC_P0_0 | BAT_RC_P0_1));
    GPIO_INPUT(0, (uint8_t)(BAT_RC_P0_0 | BAT_RC_P0_1));
    GPIO_LOW(0, BAT_RC_P0_1);
    GPIO_OUTPUT(0, BAT_RC_P0_1);
    GPIO_PULLUP_ON(0, BAT_RC_P0_0);
    delay_us(BATTERY_DISCHARGE_US);

    // Release P0.1 and charge the capacitor through its pull-up; P0.0 reads
    // the capacitor voltage.
    GPIO_PULLUP_OFF(0, (uint8_t)(BAT_RC_P0_0 | BAT_RC_P0_1));
    GPIO_INPUT(0, (uint8_t)(BAT_RC_P0_0 | BAT_RC_P0_1));
    GPIO_PULLUP_ON(0, BAT_RC_P0_1);
    delay_us(BATTERY_CHARGE_US);

    // Count how long the sense pin holds high (the capacitor holding charge).
    while (P0_0 && count < BATTERY_MAX_COUNT) {
        watchdog_kick();
        count++;
    }

    // Map the count to a 0..7 level and the critical-low flag.
    uint8_t level = (uint8_t)(((uint16_t)count * BATTERY_LEVELS) / BATTERY_MAX_COUNT);
    if (level >= BATTERY_LEVELS) {
        level = BATTERY_LEVELS - 1;
    }

    keyboard_state.battery_level_rc = level;
    keyboard_state.battery_level    = level;
    keyboard_state.low_power        = (level <= 1) ? 1 : 0;
}

#if DEBUG == 1

// Main-loop passes between diagnostic samples. The loop runs at roughly 100 Hz
// on this board, so 100 passes is about one second.
#    define BATTERY_DIAG_INTERVAL_TICKS 100

void user_battery_diag_task(void)
{
    static uint16_t ticks;

    if (++ticks < BATTERY_DIAG_INTERVAL_TICKS) {
        return;
    }
    ticks = 0;

    // The count loop times the sense pin in wall-clock, so keep the matrix/LED
    // interrupt out of it or a scan mid-measure inflates the count.
    const uint8_t ea = EA;
    EA               = 0;
    user_battery_measure();
    EA = ea;

    dprintf("bat rc=%u rf=%u lp=%u\r\n", keyboard_state.battery_level_rc, keyboard_state.battery_level_rf, keyboard_state.low_power);
}

#else

void user_battery_diag_task(void) {}

#endif
