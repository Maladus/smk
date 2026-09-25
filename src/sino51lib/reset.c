#include "reset.h"
#include "sfr.h"

uint8_t reset_status;

void reset_init(void)
{
    reset_status = RSTSTAT;
}

bool reset_was_watchdog(void)
{
    return (reset_status & _WDOF) != 0;
}
