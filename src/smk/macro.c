#include "macro.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "matrix.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stdint.h>

#    if VIAL_MACRO_BUFFER_SIZE > 0

// QMK send-string macro byte codes.
#        define SS_QMK_PREFIX 0x01
#        define SS_TAP_CODE   0x01
#        define SS_DOWN_CODE  0x02
#        define SS_UP_CODE    0x03
#        define SS_DELAY_CODE 0x04

static bool     macro_playing;
static uint16_t macro_offset;
static bool     macro_delaying;
static uint32_t macro_delay_until;

static void macro_stop(void)
{
    macro_playing     = false;
    macro_offset      = 0;
    macro_delaying    = false;
    macro_delay_until = 0;
}

static void macro_start(uint8_t index)
{
    const uint16_t size = dynamic_keymap_macro_buffer_size();
    if (size == 0) {
        return;
    }
    // Refuse to play while a buffer write is in progress: QMK requires the
    // final byte to be zero.
    if (dynamic_keymap_macro_read_byte((uint16_t)(size - 1)) != 0) {
        return;
    }

    // Macro `index` starts after `index` null separators.
    uint16_t offset    = 0;
    uint8_t  remaining = index;
    while (remaining > 0) {
        if (offset >= size) {
            return;
        }
        if (dynamic_keymap_macro_read_byte(offset) == 0) {
            remaining--;
        }
        offset++;
    }

    macro_playing     = true;
    macro_offset      = offset;
    macro_delaying    = false;
    macro_delay_until = 0;
}

void macro_init(void)
{
    macro_stop();
}

bool macro_process_record(uint16_t keycode, bool pressed)
{
    if (!IS_QK_MACRO(keycode)) {
        return false;
    }
    if (pressed) {
        macro_start((uint8_t)(keycode - QK_MACRO_0));
    }
    return true;
}

void macro_task(void)
{
    if (!macro_playing) {
        return;
    }
    if (macro_delaying) {
        if ((int32_t)(tick_ms() - macro_delay_until) < 0) {
            return;
        }
        macro_delaying = false;
    }

    while (macro_playing) {
        const uint8_t b = dynamic_keymap_macro_read_byte(macro_offset);
        if (b == 0) {
            macro_stop();
            return;
        }
        if (b != SS_QMK_PREFIX) {
            macro_offset++; // plain characters are not supported
            return;
        }

        const uint8_t code = dynamic_keymap_macro_read_byte((uint16_t)(macro_offset + 1));
        if (code == SS_TAP_CODE || code == SS_DOWN_CODE || code == SS_UP_CODE) {
            const uint8_t kc = dynamic_keymap_macro_read_byte((uint16_t)(macro_offset + 2));
            macro_offset     = (uint16_t)(macro_offset + 3);
            if (kc != 0) {
                if (code == SS_TAP_CODE) {
                    matrix_send_keycode(kc, true);
                    matrix_send_keycode(kc, false);
                } else if (code == SS_DOWN_CODE) {
                    matrix_send_keycode(kc, true);
                } else {
                    matrix_send_keycode(kc, false);
                }
            }
            return;
        }

        if (code == SS_DELAY_CODE) {
            const uint8_t d0  = dynamic_keymap_macro_read_byte((uint16_t)(macro_offset + 2));
            const uint8_t d1  = dynamic_keymap_macro_read_byte((uint16_t)(macro_offset + 3));
            macro_offset      = (uint16_t)(macro_offset + 4);
            const uint16_t ms = (uint16_t)((uint16_t)(d0 - 1) + (uint16_t)(d1 - 1) * 255u);
            macro_delaying    = true;
            macro_delay_until = tick_ms() + ms;
            return;
        }

        macro_stop();
        return;
    }
}

#    else

void macro_init(void) {}

bool macro_process_record(uint16_t keycode, bool pressed)
{
    (void)keycode;
    (void)pressed;
    return false;
}

void macro_task(void) {}

#    endif
#endif // VIAL_ENABLE
