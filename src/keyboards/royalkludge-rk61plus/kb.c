#include <stdint.h>
#include <stdbool.h>
#include "keycodes.h"
#include "kbdef.h"
#include "keyboard.h"
#include "settings.h"
#include "debug.h"
#include "report.h"
#include "usb.h"
#include "user_battery.h"

#ifdef RF_ENABLED
#    include "rf_controller.h"
#endif

extern void indicators_next_effect(void);
extern void indicators_prev_effect(void);
extern void indicators_brightness_up(void);
extern void indicators_brightness_down(void);
extern void indicators_speed_up(void);
extern void indicators_speed_down(void);

// Where keyboard reports go. RF (BLE or 2.4G) is the primary link; a short press
// on the active, connected BT channel toggles to USB, and USB is the fallback
// while no BLE channel is active.
typedef enum {
    KEYBOARD_CONN_MODE_RF  = 0,
    KEYBOARD_CONN_MODE_USB = 1,
} user_keyboard_conn_mode_t;

static user_keyboard_conn_mode_t conn_mode = KEYBOARD_CONN_MODE_RF;

// Pairing-active flag read by the Fn+Q/W/E channel indicator (indicators.c).
static volatile bool pairing_active;

bool kb_pairing_active(void)
{
    return pairing_active;
}

#ifdef RF_ENABLED
// Hold time (in main-loop kb_update() ticks) before Fn+Q/W/E starts pairing.
#    define LINK_PAIRING_HOLD_TICKS 60000
// Main-loop iterations a changed band-switch level must hold before it is
// accepted.
#    define SLIDER_DEBOUNCE_ITERS 256

static uint16_t link_hold_ticks;
static uint16_t link_hold_keycode;
static bool     link_pairing_armed;
static bool     link_press_was_active;

static rf_mode_t kb_keycode_to_rf_mode(uint16_t keycode)
{
    switch (keycode) {
        case LNK_BT1:
            return RF_MODE_BT1;
        case LNK_BT2:
            return RF_MODE_BT2;
        case LNK_BT3:
            return RF_MODE_BT3;
        default:
            return RF_MODE_2_4G;
    }
}

// P5.5: high = direct 2.4G, low = BLE.
static bool kb_band_24g(void)
{
    return BAND_SWITCH != 0;
}

// Switch the RF link and mirror it into the keyboard_state the channel
// indicator reads. `persist` records a user-chosen BT channel in settings; the
// band switch must not overwrite the remembered channel with the 2.4G slot.
static void kb_set_link(rf_mode_t link, bool persist)
{
    rf_set_link(link);
    if (persist) {
        user_settings.rf_link = (uint8_t)link;
        settings_mark_dirty();
    }
    keyboard_state.rf_link   = (uint8_t)link;
    keyboard_state.connected = 1;
    keyboard_state.paired    = 1;
}

// Apply the band switch: 2.4G pins the link, BLE restores the last BT channel.
static void kb_apply_band(void)
{
    if (kb_band_24g()) {
        kb_set_link(RF_MODE_2_4G, false);
        return;
    }

    uint8_t link = user_settings.rf_link;
    if (link == RF_MODE_2_4G || link > RF_MODE_BT3) {
        link = RF_MODE_BT1;
    }
    kb_set_link((rf_mode_t)link, false);
}
#endif

void kb_init()
{
    // Measure the battery once at boot; keyboard_init() has already zeroed
    // keyboard_state, so the RC result lands after it.
    user_battery_measure();
}

bool kb_process_record(uint16_t keycode, bool key_pressed)
{
    switch (keycode) {
        case FX_PREV:
            if (key_pressed) {
                indicators_prev_effect();
            }
            return false;
        case FX_NEXT:
            if (key_pressed) {
                indicators_next_effect();
            }
            return false;
        case SPD_DN:
            if (key_pressed) {
                indicators_speed_down();
            }
            return false;
        case SPD_UP:
            if (key_pressed) {
                indicators_speed_up();
            }
            return false;
        case BRI_DN:
            if (key_pressed) {
                indicators_brightness_down();
            }
            return false;
        case BRI_UP:
            if (key_pressed) {
                indicators_brightness_up();
            }
            return false;
        default:
            break;
    }

#ifdef RF_ENABLED
    switch (keycode) {
        case LNK_BT1:
        case LNK_BT2:
        case LNK_BT3:
            // On the 2.4G band the BLE channel keys are disabled.
            if (kb_band_24g()) {
                return false;
            }

            if (!key_pressed) {
                if (link_hold_keycode == keycode) {
                    // Released before the hold threshold: a short press. On the
                    // active, connected channel it disables BLE and falls back
                    // to USB when a host is attached.
                    if (link_pairing_armed && link_press_was_active && usb_is_configured()) {
                        conn_mode = KEYBOARD_CONN_MODE_USB;
                        rf_apply_usb_mode();
                    }
                    link_hold_keycode  = 0;
                    link_pairing_armed = false;
                    pairing_active     = false;
                }
                return false;
            }

            // Fresh press: arm the hold timer and switch channels immediately
            // unless this is the active channel, where the release decides
            // between the USB toggle and a long-press pairing.
            link_hold_keycode     = keycode;
            link_hold_ticks       = 0;
            link_pairing_armed    = true;
            link_press_was_active = (conn_mode == KEYBOARD_CONN_MODE_RF && (uint8_t)kb_keycode_to_rf_mode(keycode) == keyboard_state.rf_link && keyboard_state.connected);

            if (conn_mode == KEYBOARD_CONN_MODE_USB) {
                // Re-enable BLE on this channel.
                kb_set_link(kb_keycode_to_rf_mode(keycode), true);
                conn_mode = KEYBOARD_CONN_MODE_RF;
                rf_kbd_lazy_state_init();
            } else if (!link_press_was_active) {
                kb_set_link(kb_keycode_to_rf_mode(keycode), true);
            }
            return false;
        default:
            break;
    }
#endif
    keycode;
    key_pressed;
    return true;
}

void kb_update_switches()
{
#ifdef RF_ENABLED
    static uint16_t band_debounce;
    static int8_t   band_24g_last = -1;

    const uint8_t raw_24g = kb_band_24g();
    if (band_24g_last < 0) {
        // First read: apply the switch level without waiting out the debounce.
        band_24g_last = (int8_t)raw_24g;
        kb_apply_band();
        return;
    }

    if (raw_24g == (uint8_t)band_24g_last) {
        band_debounce = 0;
    } else if (++band_debounce >= SLIDER_DEBOUNCE_ITERS) {
        band_debounce  = 0;
        band_24g_last  = (int8_t)raw_24g;
        conn_mode      = KEYBOARD_CONN_MODE_RF;
        pairing_active = false;
        kb_apply_band();
    }
#endif
}

// RF carries reports only while a channel is actually connected; otherwise USB
// is the fallback. This is what makes the board type over USB when it is plugged
// in but no BLE/2.4G host is bound, and it matches "BLE dominates USB while a BT
// channel is connected". The active-channel toggle clears conn_mode to USB.
static bool kb_rf_active(void)
{
#ifdef RF_ENABLED
    return conn_mode == KEYBOARD_CONN_MODE_RF && keyboard_state.connected;
#else
    return false;
#endif
}

void kb_send_report(__xdata report_keyboard_t *report)
{
    if (kb_rf_active()) {
        rf_send_report(report);
        return;
    }
    usb_send_report(report);
}

void kb_send_nkro(__xdata report_nkro_t *report)
{
    if (kb_rf_active()) {
        rf_send_nkro(report);
        return;
    }
    usb_send_nkro(report);
}

void kb_send_extra(__xdata report_extra_t *report)
{
    if (kb_rf_active()) {
        rf_send_extra(report);
        return;
    }
    usb_send_extra(report);
}

void kb_update()
{
#ifdef RF_ENABLED
    if (link_pairing_armed && link_hold_keycode) {
        if (link_hold_ticks < LINK_PAIRING_HOLD_TICKS) {
            link_hold_ticks++;
        } else {
            const rf_mode_t link = kb_keycode_to_rf_mode(link_hold_keycode);
            dprintf("rf link pairing %02x\r\n", link);
            keyboard_state.paired    = 0;
            keyboard_state.connected = 0;
            pairing_active           = true;
            rf_set_link_pairing(link, &keyboard_state);
            link_pairing_armed = false;
        }
    }

    // The indicator fast-blinks while pairing; stop once the link comes up or a
    // bond is established (then it shows solid/slow-blink instead).
    if (pairing_active && (keyboard_state.connected || keyboard_state.paired)) {
        pairing_active = false;
    }

    if (conn_mode == KEYBOARD_CONN_MODE_RF) {
        rf_link_supervisor(&keyboard_state);
        rf_send_pending_flush();
        rf_blanking_tick();
    }
#endif
}
