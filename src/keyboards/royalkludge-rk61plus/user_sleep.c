#include "kbdef.h"
#include "user_sleep.h"
#include "user_init.h"
#include "gpio.h"
#include "extint.h"

#ifdef SLEEP_ENABLE

// HARDWARE NOTE: this parking sequence cannot be verified without the physical
// keyboard, and it is transcribed from the stock firmware's sleep teardown
// (0x7CC7), so the order of the writes is preserved exactly. If a pin here is
// wrong the board will not wake and will need a reflash to recover.

user_sleep_mode_t user_sleep_supported(void)
{
    // RF bring-up is a later phase; until then the board sleeps as a USB device.
    return USER_SLEEP_USB;
}

// Park the panel exactly as the stock teardown does: turn each pin group into
// outputs, then AND the port with its park mask. The five park masks are
// applied in the stock write order:
//   ANL P7,#EE  ANL P4,#92  ANL P0,#1F  ANL P7,#3F  ANL P0,#FC
static void park_panel(void)
{
    // ANL P7,#EE: P7.0 (parked-only output) + P7.4 (enable) low.
    GPIO_OUTPUT(7, (uint8_t)(PARK_P7_0 | ENABLE_P7_4));
    GPIO_WRITE(7, (uint8_t)(P7 & PARK_P7_EE_MASK));

    // Columns low so the parked matrix sources nothing (stock MOV P6,#00 and
    // ANL P5,#78).
    GPIO_DIR_WRITE(6, 0xFF);
    GPIO_WRITE(6, 0x00);
    GPIO_OUTPUT(5, (uint8_t)(KB_C8_P5_0 | KB_C9_P5_1 | KB_C10_P5_2 | KB_C11_P5_7));
    GPIO_WRITE(5, (uint8_t)(P5 & 0x78));

    // ANL P4,#92: C12/C13 + P4 enable group (P4.3/5/6) low.
    GPIO_OUTPUT(4, (uint8_t)(KB_C12_P4_0 | KB_C13_P4_2 | ENABLE_P4_3 | ENABLE_P4_5 | ENABLE_P4_6));
    GPIO_WRITE(4, (uint8_t)(P4 & PARK_P4_92_MASK));

    // ANL P0,#1F: P0.5 (enable) + P0.6/P0.7 (status) low.
    GPIO_OUTPUT(0, (uint8_t)(ENABLE_P0_5 | STATUS_P0_6 | STATUS_P0_7));
    GPIO_WRITE(0, (uint8_t)(P0 & PARK_P0_1F_MASK));

    // ANL P7,#3F: P7.6/P7.7 (control/status) low.
    GPIO_OUTPUT(7, (uint8_t)(STATUS_P7_6 | STATUS_P7_7));
    GPIO_WRITE(7, (uint8_t)(P7 & PARK_P7_3F_MASK));

    // ANL P0,#FC: P0.0/P0.1 (RC battery) low.
    GPIO_OUTPUT(0, (uint8_t)(BAT_RC_P0_0 | BAT_RC_P0_1));
    GPIO_WRITE(0, (uint8_t)(P0 & PARK_P0_FC_MASK));
}

void user_sleep_prepare(void)
{
    park_panel();
    extint_wake_arm();
}

void user_sleep_wake(void)
{
    extint_wake_disable();
    user_gpio_init();
}

#endif // SLEEP_ENABLE
