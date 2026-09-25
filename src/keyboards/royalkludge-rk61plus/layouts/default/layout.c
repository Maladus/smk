#include "kbdef.h"
#include "layout.h"
#include "user_layout.h"
#include "report.h"
#include <stdint.h>

// clang-format off

// Standard 60% ANSI (61 keys) on the 5x14 matrix. Fn is the bottom-left key.
// The physical key-to-matrix positions are a provisional standard arrangement;
// the doc traces the pin wiring but not the physical key placement, so this may
// need adjustment once the board can be probed.

#define LAYOUT_60( \
                   K00_0, K01_0, K02_0, K03_0, K04_0, K05_0, K06_0, K07_0, K08_0, K09_0, K10_0, K11_0, K12_0, K13_0, \
                   K00_1, K01_1, K02_1, K03_1, K04_1, K05_1, K06_1, K07_1, K08_1, K09_1, K10_1, K11_1, K12_1, K13_1, \
                   K00_2, K01_2, K02_2, K03_2, K04_2, K05_2, K06_2, K07_2, K08_2, K09_2, K10_2, K11_2,        K13_2, \
                   K00_3, K01_3, K02_3, K03_3, K04_3, K05_3, K06_3, K07_3, K08_3, K09_3, K10_3,              K13_3, \
                   K00_4, K01_4, K02_4,                      K05_4,               K08_4, K09_4, K10_4,       K13_4 \
                 ) { \
    { K00_0, K01_0, K02_0, K03_0, K04_0, K05_0, K06_0, K07_0, K08_0, K09_0, K10_0, K11_0, K12_0, K13_0 }, \
    { K00_1, K01_1, K02_1, K03_1, K04_1, K05_1, K06_1, K07_1, K08_1, K09_1, K10_1, K11_1, K12_1, K13_1 }, \
    { K00_2, K01_2, K02_2, K03_2, K04_2, K05_2, K06_2, K07_2, K08_2, K09_2, K10_2, K11_2, KC_NO, K13_2 }, \
    { K00_3, K01_3, K02_3, K03_3, K04_3, K05_3, K06_3, K07_3, K08_3, K09_3, K10_3, KC_NO, KC_NO, K13_3 }, \
    { K00_4, K01_4, K02_4, KC_NO, KC_NO, K05_4, KC_NO, KC_NO, K08_4, K09_4, K10_4, KC_NO, KC_NO, K13_4 } \
}

#define _BL LAYER_BASE
#define _FL LAYER_FN
#ifdef VIAL_ENABLE
#    define _FNS LAYER_FN_SHIFT
#endif

#define FN MO(_FL)

const uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    /* Keymap _BL: (Base Layer) Default Layer
     * ,------------------------------------------------------------.
     * |Esc|  1|  2|  3|  4|  5|  6|  7|  8|  9|  0|  -|  =|  Backsp|
     * |------------------------------------------------------------|
     * |Tab  |  Q|  W|  E|  R|  T|  Y|  U|  I|  O|  P|  [|  ]|     \|
     * |------------------------------------------------------------|
     * |CAPS   |  A|  S|  D|  F|  G|  H|  J|  K|  L|  ;|  '|  Return|
     * |------------------------------------------------------------|
     * |Shift   |  Z|  X|  C|  V|  B|  N|  M|  ,|  .|  /|      Shift|
     * |------------------------------------------------------------|
     * |Ctl|Gui |Alt |          Space          |Alt |Menu|Ctl |Fn  |
     * `------------------------------------------------------------'
     */
    [_BL] = LAYOUT_60(
        KC_ESC,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS, KC_EQL,  KC_BSPC,
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC, KC_RBRC, KC_BSLS,
        KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT,          KC_ENT,
        KC_LSFT, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH,          KC_RSFT,
        KC_LCTL, KC_LGUI, KC_LALT,                            KC_SPC,                    KC_RALT, KC_APP,  KC_RCTL, FN
    ),

    /* Keymap _FL: (Function Layer)
     * ,------------------------------------------------------------.
     * | ~ | F1| F2| F3| F4| F5| F6| F7| F8| F9|F10|F11|F12|    Del |
     * |------------------------------------------------------------|
     * |     |BT1|BT2|BT3|   |   |   |   |   |   |Psc|Hom|End|      |
     * |------------------------------------------------------------|
     * |      |Lef|Dow|Rig|   |   |   |   |   |   |PgU|PgD|         |
     * |------------------------------------------------------------|
     * |        |   |   |   |   |   |   |   |   |Ins|Del|           |
     * |------------------------------------------------------------|
     * |    |    |    |                         |    |    |    |    |
     * `------------------------------------------------------------'
     * BT1/BT2/BT3 = Bluetooth channel select (hold 3-5 s to pair).
     * Backlight: Fn+\ cycles the effect, Fn+[ / ] brightness, Fn+; / ' speed.
     * Special: Y PrtSc, U ScrLK, I Pause, H Insert, J Home, K PgUp,
     * N Del, M End, , PgDn. Arrows: / Up, RAlt Left, Menu Down, RCtrl Right.
     * (Matches the RK61 Plus manual and the fazreil/RK61-cheatsheet map.)
     */
    [_FL] = LAYOUT_60(
        KC_GRV,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  KC_DEL,
        _______, LNK_BT1, LNK_BT2, LNK_BT3, _______, _______, KC_PSCR, KC_SCRL, KC_PAUS, _______, _______, BRI_DN,  BRI_UP,  FX_NEXT,
        _______, _______, _______, _______, _______, _______, KC_INS,  KC_HOME, KC_PGUP, _______, SPD_DN,  SPD_UP,           _______,
        _______, _______, _______, _______, _______, _______, KC_DEL,  KC_END,  KC_PGDN, _______, KC_UP,            _______,
        _______, _______, _______,                            _______,                   KC_LEFT, KC_DOWN, KC_RGHT, _______
    ),

#ifdef VIAL_ENABLE
    /* Keymap _FNS: (Fn+Shift) number-row multimedia keys. The manual lists
     * these as Fn+F1..F12 -- the second function of the F1..F12 that Fn+1..=
     * produces -- so Fn+Shift+1..= selects them here. Everything else is
     * transparent and falls through to the Fn layer. */
    [_FNS] = LAYOUT_60(
        _______, KC_MY_COMPUTER, KC_WWW_HOME, KC_MAIL, KC_CALCULATOR, KC_MEDIA_SELECT, KC_MEDIA_STOP, KC_MEDIA_PREV_TRACK, KC_MEDIA_PLAY_PAUSE, KC_MEDIA_NEXT_TRACK, KC_AUDIO_MUTE, KC_AUDIO_VOL_DOWN, KC_AUDIO_VOL_UP, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,
        _______, _______, _______,                            _______,                   _______, _______, _______, _______
    ),
#endif
};

// clang-format on
