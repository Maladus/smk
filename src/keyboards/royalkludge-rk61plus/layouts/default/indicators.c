#include "indicators.h"
#include "kbdef.h"
#include "gpio.h"
#include "pwm.h"
#include "settings.h"
#include "tick.h"
#include "keyboard.h"
#include "led_effect.h"
#include "user_led.h"
#include <string.h>

#define LED_ROWS MATRIX_ROWS
#define LED_COLS MATRIX_COLS

#define LED_SPEED_DEFAULT 4
#define LED_SPEED_MIN     1
#define LED_SPEED_MAX     16

#define LED_BRIGHTNESS_DEFAULT 255
#define LED_BRIGHTNESS_STEP    32

// nuphy-air60 DUTY2-based duty model: DUTY1=0 drives the sink LOW at the start
// of the period and DUTY2 drives it back HIGH; the LED conducts while the sink
// is LOW, so the on-time equals DUTY2. A direct fb -> DUTY2 mapping gives
// fb=0 -> off and monotonically brighter values up to fb=255. Do NOT invert:
// mapping fb=0 to a large DUTY2 would make "off" subframes glow.
#define LED_DUTY(v) (uint16_t)(v)

// LED columns are the key-matrix column pins, driven HIGH to source current
// into the row/colour PWM sinks (the opposite level of the active-low key scan).
#define LED_C_P4_MASK (uint8_t)(KB_C12_P4_0 | KB_C13_P4_2)
#define LED_C_P5_MASK (uint8_t)(KB_C8_P5_0 | KB_C9_P5_1 | KB_C10_P5_2 | KB_C11_P5_7)
#define LED_C_P6_MASK (uint8_t)(KB_C0_P6_0 | KB_C1_P6_1 | KB_C2_P6_2 | KB_C3_P6_3 | KB_C4_P6_4 | KB_C5_P6_5 | KB_C6_P6_6 | KB_C7_P6_7)

// Fn + Q/W/E channel indicator: Q/W/E sit on row 1 at columns 1/2/3.
#define FN_ROW   1
#define FN_Q_COL 1
#define FN_W_COL 2
#define FN_E_COL 3

// keyboard_state.rf_link values (match rf_controller.h rf_mode_t; the wireless
// module is enabled in a later phase, so the constants are duplicated here).
enum {
    FN_RF_2_4G = 0x00,
    FN_RF_BT1  = 0x01,
    FN_RF_BT2  = 0x02,
    FN_RF_BT3  = 0x03,
};

// Channel indicator blink phases. status_pulse_counter advances once per full
// LED frame (LED_COLS subframes), so:
//   FN_BLINK_FAST (0x01) -> 2-frame period  (pairing)
//   FN_BLINK_SLOW (0x08) -> 16-frame period (connecting)
#define FN_BLINK_FAST 0x01
#define FN_BLINK_SLOW 0x08

#include LED_GEOMETRY_HEADER
_Static_assert(LED_GEOMETRY_ROWS == LED_ROWS && LED_GEOMETRY_COLS == LED_COLS, "generated LED geometry size does not match the key matrix");

static uint8_t led_fb[LED_ROWS][3][LED_COLS];

static uint8_t led_col;
static uint8_t led_phase;
static uint8_t regen_row;
static uint8_t regen_col;

static uint8_t anim_ctr;
static uint8_t status_pulse_counter;

static volatile bool render_dirty;

// Fn held == the momentary layer is active (Fn is the board's only MO key).
extern uint8_t action_layer;
// Pairing-active flag owned by kb.c; the RF phase drives it while a long-press
// pairing sequence runs.
extern bool kb_pairing_active(void);

void        indicators_pwm_enable(void);
void        indicators_pwm_disable(void);
static void led_regen_one(void);
static void led_columns_off(void);
static void led_column_on(uint8_t col);
static void led_set_sink(uint8_t row, uint8_t color, uint16_t duty);
static bool led_set_sinks(void);
static void led_pwm_run(void);
static void led_pwm_park(void);

void indicators_apply_defaults(void)
{
    user_settings.led_effect     = FX_SOLID_RED;
    user_settings.led_brightness = LED_BRIGHTNESS_DEFAULT;
    user_settings.led_speed      = LED_SPEED_DEFAULT;
}

void indicators_validate_settings(void)
{
    if (user_settings.led_effect > FX_OFF) {
        user_settings.led_effect = FX_OFF;
    }
    if (user_settings.led_speed < LED_SPEED_MIN) {
        user_settings.led_speed = LED_SPEED_MIN;
    }
    if (user_settings.led_speed > LED_SPEED_MAX) {
        user_settings.led_speed = LED_SPEED_MAX;
    }
}

void indicators_next_effect(void)
{
    if (++user_settings.led_effect > FX_OFF) {
        user_settings.led_effect = 0;
    }
    settings_mark_dirty();
}

void indicators_prev_effect(void)
{
    if (user_settings.led_effect == 0) {
        user_settings.led_effect = FX_OFF;
    } else {
        user_settings.led_effect--;
    }
    settings_mark_dirty();
}

void indicators_brightness_up(void)
{
    if (user_settings.led_brightness > (uint8_t)(255 - LED_BRIGHTNESS_STEP)) {
        user_settings.led_brightness = 255;
    } else {
        user_settings.led_brightness = (uint8_t)(user_settings.led_brightness + LED_BRIGHTNESS_STEP);
    }
    settings_mark_dirty();
}

void indicators_brightness_down(void)
{
    if (user_settings.led_brightness < LED_BRIGHTNESS_STEP) {
        user_settings.led_brightness = 0;
    } else {
        user_settings.led_brightness = (uint8_t)(user_settings.led_brightness - LED_BRIGHTNESS_STEP);
    }
    settings_mark_dirty();
}

void indicators_speed_up(void)
{
    if (user_settings.led_speed < LED_SPEED_MAX) {
        user_settings.led_speed++;
    }
    settings_mark_dirty();
}

void indicators_speed_down(void)
{
    if (user_settings.led_speed > LED_SPEED_MIN) {
        user_settings.led_speed--;
    }
    settings_mark_dirty();
}

void indicators_init(void)
{
    memset(led_fb, 0, sizeof(led_fb));
}

void indicators_start(void)
{
    led_col              = 0;
    led_phase            = 0;
    regen_row            = 0;
    regen_col            = 0;
    anim_ctr             = 0;
    status_pulse_counter = 0;
    render_dirty         = true; // paint an initial frame
}

void indicators_pre_update(void)
{
    // The sinks are parked and the columns blanked at the top of
    // indicators_update_step(); nothing to pre-stage here.
}

bool indicators_update_step(keyboard_state_t *keyboard, uint8_t current_step)
{
    keyboard;
    current_step;

    if (++anim_ctr >= LED_COLS) {
        anim_ctr  = 0;
        led_phase = (uint8_t)(led_phase + user_settings.led_speed);
        status_pulse_counter++;
        render_dirty = true; // animation and/or Fn indicator advanced
    }

    indicators_pwm_disable();

    bool lit = led_set_sinks(); // load the 15 row/colour duties while parked

    if (lit) {
        led_column_on(led_col); // raise this subframe's column while parked
        led_pwm_run();          // re-enable last - the selected column lights now
    }

    bool frame_wrapped = false;
    if (++led_col >= LED_COLS) {
        led_col       = 0;
        frame_wrapped = true;
    }

    return frame_wrapped;
}

void indicators_post_update(void)
{
    PWM00CON &= ~(1 << 5);
}

void indicators_render(void)
{
    if (!render_dirty) {
        return;
    }
    render_dirty = false; // cleared first: a phase bump mid-render re-arms it

    for (uint8_t i = 0; i < (uint8_t)(LED_ROWS * LED_COLS); i++) {
        led_regen_one();
    }
}

static uint8_t fn_active_col(void)
{
    switch (keyboard_state.rf_link) {
        case FN_RF_BT1:
            return FN_Q_COL;
        case FN_RF_BT2:
            return FN_W_COL;
        case FN_RF_BT3:
            return FN_E_COL;
        default:
            return 0xFF; // 2.4G: no Q/W/E key lights
    }
}

static uint8_t fn_channel_blue(void)
{
    if (kb_pairing_active()) {
        // Pairing in progress (long-press Fn+Q/W/E): fast blink.
        return (status_pulse_counter & FN_BLINK_FAST) ? 255 : 0;
    }
    if (keyboard_state.connected) {
        // Link up: solid blue.
        return 255;
    }
    // Selected but no link yet (paired or not): slow blink while it connects.
    // Fast blink is reserved for an active pairing sequence.
    return (status_pulse_counter & FN_BLINK_SLOW) ? 255 : 0;
}

static void led_regen_one(void)
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    if (user_settings.led_effect < FX_OFF) {
        uint8_t rgb[3];
        if (led_effect_rgb((led_effect_t)user_settings.led_effect, regen_row, regen_col, led_phase, user_settings.led_brightness, rgb)) {
            r = rgb[0];
            g = rgb[1];
            b = rgb[2];
        }
    }

    // Fn held: overlay the active BT channel key (Q/W/E) with its status, and
    // leave the rest of the effect running underneath.
    if (action_layer != 0 && regen_row == FN_ROW && regen_col == fn_active_col()) {
        r = 0;
        g = 0;
        b = fn_channel_blue();
    }

    led_fb[regen_row][0][regen_col] = r;
    led_fb[regen_row][1][regen_col] = g;
    led_fb[regen_row][2][regen_col] = b;

    if (++regen_col >= LED_COLS) {
        regen_col = 0;
        if (++regen_row >= LED_ROWS) {
            regen_row = 0;
        }
    }
}

static void led_columns_off(void)
{
    GPIO_HIGH(4, LED_C_P4_MASK);
    GPIO_HIGH(5, LED_C_P5_MASK);
    GPIO_HIGH(6, LED_C_P6_MASK);
}

static void led_column_on(uint8_t col)
{
    switch (col) {
        case 0:
            KB_C0 = 0;
            break;
        case 1:
            KB_C1 = 0;
            break;
        case 2:
            KB_C2 = 0;
            break;
        case 3:
            KB_C3 = 0;
            break;
        case 4:
            KB_C4 = 0;
            break;
        case 5:
            KB_C5 = 0;
            break;
        case 6:
            KB_C6 = 0;
            break;
        case 7:
            KB_C7 = 0;
            break;
        case 8:
            KB_C8 = 0;
            break;
        case 9:
            KB_C9 = 0;
            break;
        case 10:
            KB_C10 = 0;
            break;
        case 11:
            KB_C11 = 0;
            break;
        case 12:
            KB_C12 = 0;
            break;
        case 13:
            KB_C13 = 0;
            break;
    }
}

// led_fb stores colour index 0=red, 1=green, 2=blue (matching the nuphy-air60
// framebuffer), while each row's three consecutive PWM channels are ordered
// green, red, blue on the board. Translate the colour index to the physical
// sink channel here.
static void led_set_sink(uint8_t row, uint8_t color, uint16_t duty)
{
    switch (row) {
        case 0: // Esc row: PWM23-25 (green, red, blue)
            if (color == 0) {
                SET_PWM_DUTY_2(LED_SINK_R0R, duty);
            } else if (color == 1) {
                SET_PWM_DUTY_2(LED_SINK_R0G, duty);
            } else {
                SET_PWM_DUTY_2(LED_SINK_R0B, duty);
            }
            break;
        case 1: // Tab row: PWM10-12 (green, red, blue)
            if (color == 0) {
                SET_PWM_DUTY_2(LED_SINK_R1R, duty);
            } else if (color == 1) {
                SET_PWM_DUTY_2(LED_SINK_R1G, duty);
            } else {
                SET_PWM_DUTY_2(LED_SINK_R1B, duty);
            }
            break;
        case 2: // Caps row: PWM13-15 (green, red, blue)
            if (color == 0) {
                SET_PWM_DUTY_2(LED_SINK_R2R, duty);
            } else if (color == 1) {
                SET_PWM_DUTY_2(LED_SINK_R2G, duty);
            } else {
                SET_PWM_DUTY_2(LED_SINK_R2B, duty);
            }
            break;
        case 3: // Shift row: PWM03-05 (green, red, blue)
            if (color == 0) {
                SET_PWM_DUTY_2(LED_SINK_R3R, duty);
            } else if (color == 1) {
                SET_PWM_DUTY_2(LED_SINK_R3G, duty);
            } else {
                SET_PWM_DUTY_2(LED_SINK_R3B, duty);
            }
            break;
        case 4: // Ctrl row: PWM00-02 (green, red, blue)
            if (color == 0) {
                SET_PWM_DUTY_2(LED_SINK_R4R, duty);
            } else if (color == 1) {
                SET_PWM_DUTY_2(LED_SINK_R4G, duty);
            } else {
                SET_PWM_DUTY_2(LED_SINK_R4B, duty);
            }
            break;
    }
}

static bool led_set_sinks(void)
{
    uint8_t any = 0;

    for (uint8_t row = 0; row < LED_ROWS; row++) {
        for (uint8_t color = 0; color < 3; color++) {
            uint8_t v = led_fb[row][color][led_col];
            led_set_sink(row, color, LED_DUTY(v));
            any = (uint8_t)(any | v);
        }
    }

    return any != 0;
}

static void led_pwm_run(void)
{
    PWM00CON = (uint8_t)(PWM_MODE_ENABLE | PWM_SS | PWM_CLK_DIV_4);
    PWM01CON = PWM_SS;
    PWM02CON = PWM_SS;
    PWM03CON = PWM_SS;
    PWM04CON = PWM_SS;
    PWM05CON = PWM_SS;

    PWM10CON = (uint8_t)(PWM_MODE_ENABLE | PWM_SS | PWM_CLK_DIV_4);
    PWM11CON = PWM_SS;
    PWM12CON = PWM_SS;
    PWM13CON = PWM_SS;
    PWM14CON = PWM_SS;
    PWM15CON = PWM_SS;

    PWM20CON = (uint8_t)(PWM_MODE_ENABLE | PWM_SS | PWM_CLK_DIV_4);
    PWM21CON = PWM_SS;
    PWM22CON = PWM_SS;
    PWM23CON = PWM_SS;
    PWM24CON = PWM_SS;
    PWM25CON = PWM_SS;
}

static void led_pwm_park(void)
{
    PWM00CON = PWM_CON_PARKED;
    PWM01CON = PWM_CON_PARKED;
    PWM02CON = PWM_CON_PARKED;
    PWM03CON = PWM_CON_PARKED;
    PWM04CON = PWM_CON_PARKED;
    PWM05CON = PWM_CON_PARKED;

    PWM10CON = PWM_CON_PARKED;
    PWM11CON = PWM_CON_PARKED;
    PWM12CON = PWM_CON_PARKED;
    PWM13CON = PWM_CON_PARKED;
    PWM14CON = PWM_CON_PARKED;
    PWM15CON = PWM_CON_PARKED;

    PWM20CON = PWM_CON_PARKED;
    PWM21CON = PWM_CON_PARKED;
    PWM22CON = PWM_CON_PARKED;
    PWM23CON = PWM_CON_PARKED;
    PWM24CON = PWM_CON_PARKED;
    PWM25CON = PWM_CON_PARKED;
}

void indicators_pwm_enable(void)
{
    // The sinks are re-enabled by indicators_update_step() once the next column
    // and its duties are in place. matrix_scan_full() calls this after the
    // sweep; leaving the sinks parked avoids lighting the whole matrix through
    // the all-high idle columns the scan leaves behind.
}

void indicators_pwm_disable(void)
{
    led_pwm_park();
    led_columns_off();
}

// The ~5 ms flash erase runs with interrupts off, so park the scan and columns
// across it.
void settings_save_pre(void)
{
    tick_pause();
    indicators_pwm_disable();
}

void settings_save_post(void)
{
    tick_resume();
}
