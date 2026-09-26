#include "reset.h"
#include "clock.h"
#include "peripherals.h"
#include "ldo.h"
#include "watchdog.h"
#include "interrupts.h"
#include "usb.h"
#include "debug.h"
#include "console.h"
#include "matrix.h"
#include "utils.h"
#include "keyboard.h"
#include "user_init.h"
#include "indicators.h"
#include "kb.h"
#include "settings.h"
#include "tick.h"
#include "sleep.h"
#include "diag.h"
#ifdef VIAL_ENABLE
#    include "combo.h"
#    include "macro.h"
#    include "tap_dance.h"
#    include "tapping.h"
#endif
#ifdef DEBUG_SINK_UART
#    include "uart.h"
#endif
#ifdef RF_ENABLED
#    include "rf_controller.h"
#endif
#ifdef ISP_ENABLE
#    include "isp.h"
#endif

void init(void)
{
    reset_init();
    ldo_init();

    clock_init();
    peripherals_init();
#ifdef DEBUG_SINK_UART
    uart_init();
#endif

    user_init();

#ifdef ISP_ENABLE
    // Recovery: watchdog reset or Esc held at power-on hands back to the ISP
    // bootloader, so a wedged diagnostic image can still be reflashed.
    if (reset_was_watchdog()) {
        isp_jump();
    }
    if (matrix_recovery_held()) {
        isp_jump();
    }
#endif

    matrix_init();
    keyboard_init();
    usb_init();
    indicators_init();
    tick_init();

    EA = 1;
}

static void restore_settings(void)
{
    if (!settings_load()) {
        indicators_apply_defaults();
    }
    indicators_validate_settings();
}

#ifdef RF_ENABLED
static void restore_rf_link(void)
{
    rf_set_link((rf_mode_t)user_settings.rf_link);

    // Mirror the remembered channel only. connected/paired come from the RF
    // status reply: claiming a link here routes reports to RF before a host is
    // known to be bound, which stalls the USB fallback for seconds after boot.
    keyboard_state.rf_link = user_settings.rf_link;
}
#endif

void main(void)
{
    init();

    dprintf("SMK v" TOSTRING(SMK_VERSION) "\r\n");
    dprintf("KB " KEYBOARD_NAME " / " LAYOUT_NAME "\r\n");
    dprintf("DEVICE vId:" TOSTRING(USB_VID) " pId:" TOSTRING(USB_PID) "\n\r");

    kb_init();

    restore_settings();
#if DEBUG == 1
    settings_dump();
#endif

    usb_wait_for_enumeration();
    indicators_start();

    sleep_init(); // needs the board's GPIO and RF up

#ifdef RF_ENABLED
    bool rf_up = false;
#endif

    while (1) {
        watchdog_kick();

#ifdef RF_ENABLED
        if (!rf_up) {
            rf_up = true;
            rf_init();
            restore_rf_link();
        }
#endif

        kb_update_switches();
        kb_update();
        matrix_task();
#ifdef VIAL_ENABLE
        tapping_task();
        combo_task();
        tap_dance_task();
        macro_task();
#endif
        indicators_render();
        usb_task();
#ifdef VIAL_ENABLE
        vial_task();
#endif
        settings_task();
        sleep_task();

#if DEBUG == 1
        diag_task();
        interrupts_task();
        console_task();
#endif
    }
}
