#include "led_effect.h"
#include "user_led.h"

// Triangle wave: 0 at 0, 255 at 128, 0 at 256 (wraps). Drives breathing and
// the knight sweep.
static uint8_t triangle(uint8_t x)
{
    return (x < 128) ? (uint8_t)(x << 1) : (uint8_t)((uint8_t)(255 - x) << 1);
}

static uint8_t abs_diff(uint8_t a, uint8_t b)
{
    return (a > b) ? (uint8_t)(a - b) : (uint8_t)(b - a);
}

// Cheap positional hash for twinkle: stable per (row, col, phase) bucket, so a
// key flickers on/off as the phase advances without any per-key state.
static uint8_t twinkle_hash(uint8_t row, uint8_t col, uint8_t phase)
{
    return (uint8_t)((uint8_t)(row * 31u) + (uint8_t)(col * 17u) + (uint8_t)((uint8_t)(phase >> 3) * 7u));
}

uint8_t led_effect_index(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase)
{
    switch (fx) {
        case FX_HORIZONTAL:
            return (uint8_t)(user_led_axis_x(col) + phase);
        case FX_VERTICAL:
            return (uint8_t)(user_led_axis_y(row) + phase);
        case FX_GRADIENT:
            return (uint8_t)(user_led_axis_x(col) + user_led_axis_y(row));
        case FX_RAINBOW:
        case FX_BREATHING:
            return phase;
        case FX_RADIAL:
        default:
            return (uint8_t)(user_led_radial(row, col) + phase);
    }
}

void led_color_wheel(uint8_t index, uint8_t out[3])
{
    if (index < 85) {
        out[0] = (uint8_t)(255 - index * 3);
        out[1] = 0;
        out[2] = (uint8_t)(index * 3);
    } else if (index < 170) {
        index  = (uint8_t)(index - 85);
        out[0] = 0;
        out[1] = (uint8_t)(index * 3);
        out[2] = (uint8_t)(255 - index * 3);
    } else {
        index  = (uint8_t)(index - 170);
        out[0] = (uint8_t)(index * 3);
        out[1] = (uint8_t)(255 - index * 3);
        out[2] = 0;
    }
}

// Predefined colours the colour key cycles through. Slot 0 is white so the
// default static solid is white, and the full wheel (including white) is
// reachable from the single static effect via Fn+..
static const __code uint8_t led_color_palette_table[LED_COLOR_COUNT][3] = {
    {255, 255, 255}, // white (QMK RGB_WHITE)
    {255, 0, 0},     // red   (QMK RGB_RED)
    {255, 128, 0},   // orange (QMK RGB_ORANGE)
    {255, 217, 0},   // gold   (QMK RGB_GOLD - less green than pure yellow)
    {0, 255, 0},     // green  (QMK RGB_GREEN)
    {0, 255, 128},   // spring green (QMK RGB_SPRINGGREEN)
    {0, 255, 255},   // cyan   (QMK RGB_CYAN)
    {0, 128, 255},   // azure
    {0, 0, 255},     // blue   (QMK RGB_BLUE)
    {160, 0, 255},   // violet
};

void led_color_palette(uint8_t index, uint8_t out[3])
{
    index  = (uint8_t)(index % LED_COLOR_COUNT);
    out[0] = led_color_palette_table[index][0];
    out[1] = led_color_palette_table[index][1];
    out[2] = led_color_palette_table[index][2];
}

// Core effect evaluation: fills `out` with the effect's colour and returns the
// per-key intensity (0..255) through `intensity`. `color` is the palette slot
// (only the colourable effects use it).
//
// The positional effects (radial/horizontal/vertical/gradient) map position to
// a wheel index; the temporal ones (breathing/rainbow/snake/knight/twinkle)
// derive their output from `phase` alone, so they need no per-key state.
static void effect_color(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t color, uint8_t out[3], uint8_t *intensity)
{
    *intensity = 255;

    switch (fx) {
        case FX_SOLID:
            led_color_palette(color, out);
            return;

        case FX_BREATHING:
            led_color_palette(color, out);
            *intensity = triangle(phase);
            return;

        case FX_RAINBOW:
            led_color_wheel(phase, out);
            return;

        case FX_GRADIENT:
            led_color_wheel((uint8_t)(user_led_axis_x(col) + user_led_axis_y(row)), out);
            return;

        case FX_SNAKE:
            // A bright band sweeps along the columns and wraps.
            if ((uint8_t)(user_led_axis_x(col) + phase) < 32u) {
                led_color_palette(color, out);
            } else {
                out[0] = out[1] = out[2] = 0;
            }
            return;

        case FX_KNIGHT: {
            // A single bright segment bounces left <-> right.
            const uint8_t pos = triangle(phase);
            if (abs_diff(user_led_axis_x(col), pos) < 32u) {
                led_color_palette(color, out);
            } else {
                out[0] = out[1] = out[2] = 0;
            }
            return;
        }

        case FX_TWINKLE:
            if (twinkle_hash(row, col, phase) & 0x80u) {
                led_color_palette(color, out);
            } else {
                out[0] = out[1] = out[2] = 0;
            }
            return;

        case FX_RADIAL:
        case FX_HORIZONTAL:
        case FX_VERTICAL:
        default:
            // Animated positional effects keep their rainbow; the palette slot
            // shifts the wheel phase.
            led_color_wheel((uint8_t)(led_effect_index(fx, row, col, phase) + color * (256 / LED_COLOR_COUNT)), out);
            return;
    }
}

static void scale_rgb(uint8_t out[3], uint8_t brightness)
{
    out[0] = (uint8_t)(((uint16_t)out[0] * brightness + 255) >> 8);
    out[1] = (uint8_t)(((uint16_t)out[1] * brightness + 255) >> 8);
    out[2] = (uint8_t)(((uint16_t)out[2] * brightness + 255) >> 8);
}

bool led_effect_rgb(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t out[3])
{
    if (fx >= FX_OFF) {
        return false;
    }
    uint8_t intensity;
    effect_color(fx, row, col, phase, 0, out, &intensity);
    scale_rgb(out, (uint8_t)(((uint16_t)brightness * intensity + 255) >> 8));
    return true;
}

bool led_effect_rgb_colored(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t color, uint8_t out[3])
{
    if (fx >= FX_OFF) {
        return false;
    }
    uint8_t intensity;
    effect_color(fx, row, col, phase, color, out, &intensity);
    scale_rgb(out, (uint8_t)(((uint16_t)brightness * intensity + 255) >> 8));
    return true;
}

bool led_effect_mono(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t *out)
{
    if (fx >= FX_OFF) {
        return false;
    }

    if (fx == FX_SOLID || fx == FX_RAINBOW) {
        *out = 255; // static full brightness
    } else if (fx == FX_BREATHING) {
        *out = triangle(phase);
    } else if (fx == FX_SNAKE) {
        *out = ((uint8_t)(user_led_axis_x(col) + phase) < 32u) ? 255 : 0;
    } else if (fx == FX_KNIGHT) {
        *out = (abs_diff(user_led_axis_x(col), triangle(phase)) < 32u) ? 255 : 0;
    } else if (fx == FX_TWINKLE) {
        *out = (twinkle_hash(row, col, phase) & 0x80u) ? 255 : 0;
    } else {
        const uint8_t x = led_effect_index(fx, row, col, phase);
        *out            = (x < 128) ? (uint8_t)(x << 1) : (uint8_t)((uint8_t)(255 - x) << 1);
    }
    return true;
}
