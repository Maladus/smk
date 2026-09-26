#include "key_override.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "matrix.h"
#    include "report.h"

#    include <stdbool.h>
#    include <stdint.h>

#    if VIAL_KEY_OVERRIDE_ENTRIES > 0

static uint8_t  active_override; // 0xFF when none
static uint16_t active_trigger;

void key_override_init(void)
{
    active_override = 0xFF;
    active_trigger  = 0;
}

static bool ko_matches(const vial_key_override_entry_t *e, uint16_t keycode, uint8_t layer, uint8_t mods)
{
    if (!(e->options & vial_ko_enabled)) {
        return false;
    }
    if (e->trigger != keycode) {
        return false;
    }
    if (e->layers != 0 && !(e->layers & (uint16_t)(1u << layer))) {
        return false;
    }
    if ((mods & e->trigger_mods) != e->trigger_mods) {
        return false;
    }
    if (mods & e->negative_mod_mask) {
        return false;
    }
    return true;
}

bool key_override_process_record(uint16_t keycode, uint8_t layer, bool pressed)
{
    if (pressed) {
        if (active_override != 0xFF) {
            return false; // one override at a time
        }
        const uint8_t n    = dynamic_keymap_key_override_count();
        const uint8_t mods = get_mods();
        for (uint8_t i = 0; i < n; i++) {
            vial_key_override_entry_t e;
            if (dynamic_keymap_get_key_override(i, &e) != 0) {
                continue;
            }
            if (ko_matches(&e, keycode, layer, mods)) {
                active_override = i;
                active_trigger  = keycode;
                matrix_send_keycode(e.replacement, true);
                return true;
            }
        }
        return false;
    }

    if (active_override != 0xFF && keycode == active_trigger) {
        vial_key_override_entry_t e;
        if (dynamic_keymap_get_key_override(active_override, &e) == 0) {
            matrix_send_keycode(e.replacement, false);
        }
        active_override = 0xFF;
        return true;
    }
    return false;
}

#    else

void key_override_init(void) {}

bool key_override_process_record(uint16_t keycode, uint8_t layer, bool pressed)
{
    (void)keycode;
    (void)layer;
    (void)pressed;
    return false;
}

#    endif
#endif // VIAL_ENABLE
