#include "kbdef.h"
#include "user_init.h"
#include "gpio.h"

// Port setup is transcribed from the stock firmware's boot routine (0xA583).
// The boot port writes reproduce the stock values exactly:
//   MOV P0,#24   MOV P4,#FD   MOV P7,#10
// with P1/P2/P3 cleared (LED sinks off), P5 columns idle-high and P6 columns
// idle-high.

void user_init()
{
    user_gpio_init();

    GPIO_WRITE(0, BOOT_P0_VALUE); // 0x24: P0.2 (WAKE) + P0.5 (enable) high
    GPIO_WRITE(1, 0x00);          // PWM row/colour sinks off
    GPIO_WRITE(2, 0x00);
    GPIO_WRITE(3, 0x00);
    GPIO_WRITE(4, BOOT_P4_VALUE); // 0xFD: columns + enable group high, ACK input
    GPIO_WRITE(5, 0x87);          // P5 columns C8-C11 idle-high
    GPIO_WRITE(6, 0xFF);          // P6 columns C0-C7 idle-high
    GPIO_WRITE(7, BOOT_P7_VALUE); // 0x10: P7.4 (enable) high
}

void user_gpio_init()
{
    // Directions, matching the stock boot routine (0xA583): 1 = output.
    // P0: WAKE (P0.2), MOSI (P0.4), enable (P0.5) output; RC (P0.0/1), MISO
    // (P0.3) and status (P0.6/7) stay input.
    GPIO_DIR_WRITE(0, 0x34);
    // P1/P2/P3.0-5 are the LED PWM row/colour sinks (parked low; LED deferred).
    GPIO_DIR_WRITE(1, 0x3F);
    GPIO_DIR_WRITE(2, 0x3F);
    GPIO_DIR_WRITE(3, 0x3F);
    // P4: C12/C13, enable (P4.3/5/6), CS (P4.4) and SCK (P4.7) output; ACK
    // (P4.1) stays input.
    GPIO_DIR_WRITE(4, 0xFD);
    // P5: columns C8-C10 (P5.0-2) and C11 (P5.7) output; rows R3/R4 (P5.3/4)
    // and switches (P5.5/6) stay input.
    GPIO_DIR_WRITE(5, 0x87);
    // P6: columns C0-C7.
    GPIO_DIR_WRITE(6, 0xFF);
    // P7: enable (P7.4) and control (P7.6) output; rows R0-R2 (P7.1-3), status
    // (P7.5) and control/status (P7.7) stay input.
    GPIO_DIR_WRITE(7, 0x50);

    // Pull-ups, matching the stock boot routine (0xA59B).
    GPIO_PULLUP_WRITE(0, 0xE0);
    GPIO_PULLUP_WRITE(1, 0x3F);
    GPIO_PULLUP_WRITE(2, 0x3F);
    GPIO_PULLUP_WRITE(3, 0x3F);
    GPIO_PULLUP_WRITE(4, 0x6D);
    GPIO_PULLUP_WRITE(5, 0xFF);
    GPIO_PULLUP_WRITE(6, 0xFF);
    GPIO_PULLUP_WRITE(7, 0xDF);

    // 25 mA drive for the ports that expose a drive-strength register, matching
    // the stock boot routine (0xA5C9).
    DRVCON = DRVCON_UNLOCK_P1;
    P1DRV  = GPIO_DRIVE_25MA;
    DRVCON = DRVCON_UNLOCK_P2;
    P2DRV  = GPIO_DRIVE_25MA;
    DRVCON = DRVCON_UNLOCK_P3;
    P3DRV  = GPIO_DRIVE_25MA;
    DRVCON = DRVCON_UNLOCK_P5;
    P5DRV  = GPIO_DRIVE_25MA;
    DRVCON = DRVCON_LOCK;
}
