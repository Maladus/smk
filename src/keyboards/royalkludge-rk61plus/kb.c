#include <stdint.h>
#include <stdbool.h>
#include "report.h"
#include "usb.h"
#include "kbdef.h"
#include "user_battery.h"

// RF routing (report fan-out, Fn+Q/W/E link keys, band switch handling) is a
// later phase. Until then every key passes straight through and all reports go
// over USB.

void kb_init()
{
    // Measure the battery once at boot; keyboard_init() has already zeroed
    // keyboard_state, so the RC result lands after it.
    user_battery_measure();
}

bool kb_process_record(uint16_t keycode, bool key_pressed)
{
    keycode;
    key_pressed;
    return true;
}

void kb_send_report(__xdata report_keyboard_t *report)
{
    usb_send_report(report);
}

void kb_send_nkro(__xdata report_nkro_t *report)
{
    usb_send_nkro(report);
}

void kb_send_extra(__xdata report_extra_t *report)
{
    usb_send_extra(report);
}
