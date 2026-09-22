#include <stdbool.h>

// Default: always bring the radio up. Keyboards with an on/off switch that gates
// the radio (e.g. the RK61 Plus) override this in their kb.c, which keeps this
// object out of the link.
bool kb_radio_enabled(void)
{
    return true;
}
