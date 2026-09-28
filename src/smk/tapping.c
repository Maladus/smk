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

// A press that already resolved as a tap, tracked so the physical release is
// handled: the keycode is either already tapped or still held down for the host
// to auto-repeat.
#    define TAP_KIND_NONE 0u
#    define TAP_KIND_ONLY 1u
#    define TAP_KIND_HELD 2u

// Recent taps remembered for the quick tap check.
#    define QUICK_TAP_HISTORY 4

typedef struct {
    bool     used;
    bool     undecided;   // waiting for the tapping term
    bool     hold_active; // decided hold; the layer/mod stays on until release
    bool     interrupted; // another key went down while this key was down
    uint8_t  tap_kind;    // TAP_KIND_* when the press resolved as a tap
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

// Flow tap (QMK's FLOW_TAP_TERM): while typing at speed a tap/hold key that
// follows another key within the flow window resolves as a tap on press, so the
// letter appears without waiting for the term. Both the tap keycode and the key
// before it must be a "flow tap key" (the alpha block or space).
static uint32_t last_key_ms;
static uint16_t last_key_kc;

// Quick tap (QMK's QUICK_TAP_TERM): pressing a tap/hold key again soon after
// tapping it repeats the tap instead of holding, which keeps auto-repeat on
// keys like backspace. QMK measures the window from the release of the tap,
// which is exactly when these are stamped.
static uint8_t  quick_row[QUICK_TAP_HISTORY] = {0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t  quick_col[QUICK_TAP_HISTORY] = {0xFF, 0xFF, 0xFF, 0xFF};
static uint32_t quick_ms[QUICK_TAP_HISTORY];
static uint8_t  quick_next;

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

static bool is_flow_tap_key(uint16_t kc)
{
    return (kc >= KC_A && kc <= KC_Z) || kc == KC_COMM || kc == KC_DOT || kc == KC_SCLN || kc == KC_SLSH || kc == KC_SPC;
}

// Remember the key that was typed last, so the next tap/hold key can tell
// whether it is in a typing flow.
static void note_flow_key(uint16_t kc)
{
    last_key_ms = tick_ms();
    last_key_kc = kc;
}

static void note_tap(uint8_t row, uint8_t col)
{
    quick_row[quick_next] = row;
    quick_col[quick_next] = col;
    quick_ms[quick_next]  = tick_ms();
    quick_next            = (uint8_t)((quick_next + 1) % QUICK_TAP_HISTORY);
}

// True when a tap/hold key must resolve as a tap because it followed a typed
// key closely. Skipped while a buffered event is replayed: that is how two
// mod-tap keys pressed together stay a chord instead of two taps.
static bool flow_tap_applies(uint16_t tap_keycode)
{
    const uint16_t term = dynamic_keymap_flow_tap_term();
    if (term == 0 || replaying || !is_flow_tap_key(tap_keycode) || !is_flow_tap_key(last_key_kc)) {
        return false;
    }
    return (tick_ms() - last_key_ms) < term;
}

// True when this key was tapped a moment ago, so pressing it again repeats the
// tap. QMK caps the quick tap window at the tapping term.
static bool quick_tap_applies(uint8_t row, uint8_t col)
{
    const uint16_t tapping = dynamic_keymap_tapping_term();
    uint16_t       term    = dynamic_keymap_quick_tap_term();
    if (term > tapping) {
        term = tapping;
    }
    if (term == 0) {
        return false;
    }
    for (uint8_t i = 0; i < QUICK_TAP_HISTORY; i++) {
        if (quick_row[i] == row && quick_col[i] == col && (tick_ms() - quick_ms[i]) < term) {
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
    note_tap(t->row, t->col);
    if (t->tap_toggle) {
        matrix_layer_toggle(t->hold_layer);
    } else {
        note_flow_key(t->tap_keycode);
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
    t->interrupted = false;
    t->tap_kind    = TAP_KIND_NONE;
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

// Resolve a tap/hold key as a tap the moment it is pressed, either because the
// key before it was typed a moment ago (flow tap) or because this key was tapped
// a moment ago (quick tap). Quick tap keeps the keycode down so the host
// auto-repeats it; a flow tap sends the tap and its release at once. The slot
// only exists so the physical release is handled. False when there is no room to
// track it, and the caller falls back to a normal tap/hold decision.
static bool tap_press(uint8_t row, uint8_t col, uint16_t tap_keycode, bool hold_key)
{
    const int8_t i = slot_alloc();
    if (i < 0) {
        return false;
    }

    tap_hold_t *t  = &slots[i];
    t->used        = true;
    t->undecided   = false;
    t->hold_active = false;
    t->interrupted = false;
    t->tap_kind    = hold_key ? TAP_KIND_HELD : TAP_KIND_ONLY;
    t->tap_toggle  = false;
    t->mod_tap     = false;
    t->row         = row;
    t->col         = col;
    t->hold_layer  = 0;
    t->hold_mods   = 0;
    t->tap_keycode = tap_keycode;
    t->start_ms    = tick_ms();

    note_flow_key(tap_keycode);
    if (hold_key) {
        matrix_send_keycode(tap_keycode, true);
    } else {
        matrix_tap_keycode(tap_keycode);
    }
    return true;
}

// Retro tapping (QMK's RETRO_TAPPING): a tap/hold key that was held past the
// term with nothing else pressed sends its tap keycode on release, instead of
// leaving the hold as the answer. The modifier or layer was live while the key
// was down, so mouse chords still work.
static void retro_tap(uint8_t i)
{
    tap_hold_t *t = &slots[i];
    note_tap(t->row, t->col);
    if (t->tap_toggle) {
        matrix_layer_toggle(t->hold_layer);
    } else {
        note_flow_key(t->tap_keycode);
        matrix_tap_keycode(t->tap_keycode);
    }
}

bool tapping_process_record(uint8_t row, uint8_t col, uint16_t keycode, bool pressed)
{
    const int8_t mine = slot_find(row, col);
    if (mine >= 0) {
        tap_hold_t *t = &slots[mine];
        if (t->tap_kind != TAP_KIND_NONE) {
            if (!pressed) {
                if (t->tap_kind == TAP_KIND_HELD) {
                    matrix_send_keycode(t->tap_keycode, false);
                }
                t->used = false;
            }
            return true;
        }
        if (t->undecided) {
            if (!pressed) {
                if ((tick_ms() - t->start_ms) < dynamic_keymap_tapping_term()) {
                    decide_tap((uint8_t)mine);
                } else if (dynamic_keymap_retro_tapping() && !t->interrupted) {
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
                const bool retro = dynamic_keymap_retro_tapping() && !t->interrupted;
                release_hold((uint8_t)mine);
                if (retro) {
                    retro_tap((uint8_t)mine);
                }
            }
            return true;
        }
        t->used = false; // stale entry; should not happen
    }

    // Another key went down while a tap/hold key was still down. That is an
    // interrupt: it settles the tap/hold key as a hold and turns off retro
    // tapping for it.
    if (pressed) {
        for (uint8_t i = 0; i < TAPPING_SLOTS; i++) {
            if (slots[i].used && slots[i].tap_kind == TAP_KIND_NONE) {
                slots[i].interrupted = true;
            }
        }
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
        const uint16_t tap = QK_MOD_TAP_GET_TAP_KEYCODE(keycode);
        if (quick_tap_applies(row, col) && tap_press(row, col, tap, true)) {
            return true;
        }
        if (flow_tap_applies(tap) && tap_press(row, col, tap, false)) {
            return true;
        }
        return arm(row, col, 0, QK_MOD_TAP_GET_MODS(keycode), tap, false, true);
    }

    if (pressed && IS_QK_LAYER_TAP(keycode)) {
        const uint16_t tap = QK_LAYER_TAP_GET_TAP_KEYCODE(keycode);
        if (quick_tap_applies(row, col) && tap_press(row, col, tap, true)) {
            return true;
        }
        if (flow_tap_applies(tap) && tap_press(row, col, tap, false)) {
            return true;
        }
        return arm(row, col, QK_LAYER_TAP_GET_LAYER(keycode), 0, tap, false, false);
    }

    if (pressed && IS_QK_LAYER_TAP_TOGGLE(keycode)) {
        return arm(row, col, QK_LAYER_TAP_TOGGLE_GET_LAYER(keycode), 0, 0, true, false);
    }

    if (pressed) {
        note_flow_key(keycode);
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
