#include "tick.h"
#include "systick.h"
#include "matrix.h"
#include "indicators.h"
#include "keyboard.h"
#include "sleep.h"
#include "kbdef.h"
#include <stdbool.h>
#include <stdint.h>

// subframes must outnumber the scans, or the refresh rate is the scan rate divided by the subframe count.
#ifndef LED_SUBFRAMES_PER_SCAN
#    define LED_SUBFRAMES_PER_SCAN 1
#endif

static volatile bool    scan_due;
static volatile uint8_t subframes_since_scan;

#ifdef VIAL_ENABLE
// Milliseconds for the tapping engine. Each timer2 period is 100 us (matrix
// scan) or 400 us (LED subframe), so accumulate those and carry into ms.
static volatile uint16_t tick_us_accum;
static volatile uint32_t tick_ms_counter;
#endif

void tick_init(void)
{
    scan_due             = true;
    subframes_since_scan = 0;

    systick_init();
}

static void run_matrix_scan(void)
{
    systick_arm(SYSTICK_SLOT_MATRIX_SCAN);
    matrix_scan_full();
}

static void run_led_subframe(void)
{
    systick_arm(SYSTICK_SLOT_LED_SUBFRAME);

    indicators_pre_update();
    const bool frame_wrapped = indicators_update_step(&keyboard_state, 0);
    indicators_post_update();

    sleep_note_frame(frame_wrapped);
}

void tick_dispatch(void)
{
    if (scan_due) {
        scan_due             = false;
        subframes_since_scan = 0;
        run_matrix_scan();
#ifdef VIAL_ENABLE
        tick_us_accum += 100;
        if (tick_us_accum >= 1000) {
            tick_us_accum -= 1000;
            tick_ms_counter++;
        }
#endif
        return;
    }

    run_led_subframe();
#ifdef VIAL_ENABLE
    tick_us_accum += 400;
    if (tick_us_accum >= 1000) {
        tick_us_accum -= 1000;
        tick_ms_counter++;
    }
#endif

    if (++subframes_since_scan >= LED_SUBFRAMES_PER_SCAN) {
        scan_due = true;
    }
}

#ifdef VIAL_ENABLE
uint32_t tick_ms(void)
{
    uint32_t value;
    __critical
    {
        value = tick_ms_counter;
    }
    return value;
}
#endif

void tick_pause(void)
{
    systick_pause();
}

void tick_resume(void)
{
    systick_resume();
}
