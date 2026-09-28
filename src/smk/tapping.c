#include "tapping.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "keycodes.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "report.h"
#    include "tick.h"

#    include <stdbool.h>
#    include <stdint.h>

// One slot per tap/hold key that is down. A single global slot used to leak the
// modifier or layer of the first key as soon as a second tap/hold key was
// pressed (rolling home-row mods, or a re-press of the same key), because the
// second press overwrote the bookkeeping the first key's release needed. Each
// press now owns its slot, so a release only clears what its own press set.
#    define TAPPING_SLOTS 8

// Events held while a decision is pending. Enough for a fast roll plus the
// release of the tap/hold key itself; older events are dropped if it fills.
#    define TAPPING_BUFFER 8

typedef struct {
    bool     used;
    bool     undecided;   // waiting for the tapping term
    bool     hold_active; // decided hold; the layer/mod stays on until release
    bool     tap_toggle;  // tap toggles the layer (TT) instead of tapping a keycode
    bool     mod_tap;     // hold applies a modifier (MT) instead of a layer
    uint8_t  row;
    uint8_t  col;
    uint8_t  hold_layer;
    uint8_t  hold_mods;
    uint16_t tap_keycode;
    uint32_t start_ms;
} tap_hold_t;

static tap_hold_t slots[TAPPING_SLOTS];

// Ring buffer of the events that arrived while a decision was pending.
static uint8_t buf_row[TAPPING_BUFFER];
static uint8_t buf_col[TAPPING_BUFFER];
static bool    buf_pressed[TAPPING_BUFFER];
static uint8_t buf_head;
static uint8_t buf_count;
static bool    replaying;

static int8_t slot_find(uint8_t row, uint8_t col)
{
    for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
        if (slots[i].used && slots[i].row == row && slots[i].col == col) {
            return (int8_t)i;
        }
    }
    return -1;
}

static int8_t slot_alloc(void)
{
    for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
        if (!slots[i].used) {
            return (int8_t)i;
        }
    }
    return -1;
}

static bool any_undecided(void)
{
    for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
        if (slots[i].used && slots[i].undecided) {
            return true;
        }
    }
    return false;
}

static void buffer_event(uint8_t row, uint8_t col, bool pressed)
{
    if (buf_count >= TAPPING_BUFFER) {
        return;
    }
    const uint8_t tail = (uint8_t)((buf_head + buf_count) % TAPPING_BUFFER);
    buf_row[tail]      = row;
    buf_col[tail]      = col;
    buf_pressed[tail]  = pressed;
    buf_count++;
}

// Replays buffered events through the tap/hold decision first, so a second
// tap/hold key that was buffered is armed instead of being dropped. Events that
// arrive during the drain are appended and picked up by the same loop, which
// keeps their order. The guard stops a re-entrant call from draining twice.
static void replay(void)
{
    if (replaying) {
        return;
    }
    replaying = true;
    while (buf_count > 0) {
        const uint8_t row     = buf_row[buf_head];
        const uint8_t col     = buf_col[buf_head];
        const bool    pressed = buf_pressed[buf_head];
        buf_head              = (uint8_t)((buf_head + 1) % TAPPING_BUFFER);
        buf_count--;
        matrix_replay_key(row, col, pressed);
    }
    replaying = false;
}

static void decide_hold(uint8_t i)
{
    tap_hold_t *t  = &slots[i];
    t->undecided   = false;
    t->hold_active = true;
    if (t->mod_tap) {
        add_mods(MODS_5BIT_TO_8BIT(t->hold_mods));
        send_keyboard_report();
    } else {
        matrix_layer_activate(t->hold_layer);
    }
    replay();
}

static void decide_tap(uint8_t i)
{
    tap_hold_t *t = &slots[i];
    t->undecided  = false;
    t->used       = false; // a tap has nothing left to track
    if (t->tap_toggle) {
        matrix_layer_toggle(t->hold_layer);
    } else {
        matrix_tap_keycode(t->tap_keycode);
    }
    replay();
}

// Drops what this slot contributed. Mods and layers shared with another active
// hold (Ctrl on both F and J, two keys on the same layer) stay on until the last
// holder releases.
static void release_hold(uint8_t i)
{
    tap_hold_t *t = &slots[i];

    if (t->mod_tap) {
        uint8_t still_held = 0;
        for (uint8_t j = 0; j < TAPPING_SLOTS; j++) {
            if (j != i && slots[j].used && slots[j].hold_active && slots[j].mod_tap) {
                still_held |= MODS_5BIT_TO_8BIT(slots[j].hold_mods);
            }
        }
        del_mods((uint8_t)(MODS_5BIT_TO_8BIT(t->hold_mods) & ~still_held));
        send_keyboard_report();
    } else {
        bool still_held = false;
        for (uint8_t j = 0; j < TAPPING_SLOTS; j++) {
            if (j != i && slots[j].used && slots[j].hold_active && !slots[j].mod_tap && slots[j].hold_layer == t->hold_layer) {
                still_held = true;
            }
        }
        if (!still_held) {
            matrix_layer_deactivate(t->hold_layer);
        }
    }

    t->hold_active = false;
    t->used        = false;
}

// Resolves every pending hold as a hold. Replay can arm another slot, so the
// scan repeats until nothing is left undecided.
static void resolve_all_holds(void)
{
    for (uint8_t pass = 0; pass < TAPPING_SLOTS; pass++) {
        bool decided = false;
        for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
            if (slots[i].used && slots[i].undecided) {
                decide_hold(i);
                decided = true;
            }
        }
        if (!decided) {
            return;
        }
    }
}

// Arm the tap/hold decision for an LT (tap_keycode), TT (tap_toggle) or MT
// (mod_tap) key. Returns false when the table is full, so the caller can let
// the key through untouched instead of swallowing it.
static bool arm(uint8_t row, uint8_t col, uint8_t layer, uint8_t mods, uint16_t tap_keycode, bool tap_toggle, bool mod_tap)
{
    const int8_t i = slot_alloc();
    if (i < 0) {
        return false;
    }
    if (layer >= VIAL_LAYERS) {
        layer = (uint8_t)(VIAL_LAYERS - 1);
    }

    tap_hold_t *t  = &slots[i];
    t->used        = true;
    t->undecided   = true;
    t->hold_active = false;
    t->tap_toggle  = tap_toggle;
    t->mod_tap     = mod_tap;
    t->row         = row;
    t->col         = col;
    t->hold_layer  = layer;
    t->hold_mods   = mods;
    t->tap_keycode = tap_keycode;
    t->start_ms    = tick_ms();
    return true;
}

bool tapping_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    const int8_t mine = slot_find(row, col);
    if (mine >= 0) {
        tap_hold_t *t = &slots[mine];
        if (t->undecided) {
            if (!pressed) {
                if ((tick_ms() - t->start_ms) < dynamic_keymap_tapping_term()) {
                    decide_tap((uint8_t)mine);
                } else {
                    // Held past the term, so this is a hold. Replay the buffered
                    // events under it, then drop it again: the key is already up,
                    // so nothing may stay latched (tapping_task usually decides
                    // this earlier; this is the path when it did not run).
                    decide_hold((uint8_t)mine);
                    release_hold((uint8_t)mine);
                }
            }
            // A duplicate press (bounce or re-press) keeps the original
            // decision pending instead of re-arming and losing the hold.
            return true;
        }
        if (t->hold_active) {
            if (!pressed) {
                release_hold((uint8_t)mine);
            }
            return true;
        }
        t->used = false; // stale entry; should not happen
    }

    // This key has no decision of its own. Its event may settle the pending
    // holds of other keys.
    if (any_undecided()) {
        if (pressed && dynamic_keymap_hold_on_other_key_press()) {
            resolve_all_holds();
        } else if (!pressed && dynamic_keymap_permissive_hold()) {
            resolve_all_holds();
        } else if (!replaying) {
            // Defer until a decision is made. A replayed event must not wait
            // again, or the replay loop would spin on it forever.
            buffer_event(row, col, pressed);
            return true;
        }
    }

    if (pressed && IS_QK_MOD_TAP(keycode)) {
        return arm(row, col, 0, QK_MOD_TAP_GET_MODS(keycode), QK_MOD_TAP_GET_TAP_KEYCODE(keycode), false, true);
    }

    if (pressed && IS_QK_LAYER_TAP(keycode)) {
        return arm(row, col, QK_LAYER_TAP_GET_LAYER(keycode), 0, QK_LAYER_TAP_GET_TAP_KEYCODE(keycode), false, false);
    }

    if (pressed && IS_QK_LAYER_TAP_TOGGLE(keycode)) {
        return arm(row, col, QK_LAYER_TAP_TOGGLE_GET_LAYER(keycode), 0, 0, true, false);
    }

    return false;
}

void tapping_task(void)
{
    for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
        if (slots[i].used && slots[i].undecided && (tick_ms() - slots[i].start_ms) >= dynamic_keymap_tapping_term()) {
            decide_hold(i);
        }
    }
}

#endif // VIAL_ENABLE
