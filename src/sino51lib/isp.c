#include "isp.h"
#include "clock.h"
#include "sfr.h"

void isp_jump()
{
    // The bootloader entry does not bring up the clock tree itself: it assumes
    // the running application left a PLL the USB block can work off. A stopped
    // PLL makes the bootloader assert its USB pull-up without ever answering
    // enumeration, which looks like a hang. Restart the clock before handing
    // over, the same way the cold ISP path does.
    clock_init();

    // clang-format off
    __asm
    clr IE.7
    mov B, #ISP_KEY_B
    mov A, #ISP_KEY_A
    ljmp ISP_ENTRY
    __endasm;
    // clang-format on
}
