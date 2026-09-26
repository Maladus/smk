#include "led_effect.h"
#include "user_led.h"

uint8_t led_effect_index(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase)
{
    switch (fx) {
        case FX_HORIZONTAL:
            return (uint8_t)(user_led_axis_x(col) + phase);
        case FX_VERTICAL:
            return (uint8_t)(user_led_axis_y(row) + phase);
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

// Predefined colours the colour key cycles through. Slot 0 is red so the
// default solid effect is unchanged.
static const __code uint8_t led_color_palette_table[LED_COLOR_COUNT][3] = {
    {255, 0, 0},   // red
    {255, 96, 0},  // orange
    {255, 200, 0}, // yellow
    {0, 255, 0},   // green
    {0, 255, 160}, // spring
    {0, 255, 255}, // cyan
    {0, 128, 255}, // azure
    {0, 0, 255},   // blue
    {160, 0, 255}, // violet
    {255, 0, 200}, // magenta
};

void led_color_palette(uint8_t index, uint8_t out[3])
{
    index  = (uint8_t)(index % LED_COLOR_COUNT);
    out[0] = led_color_palette_table[index][0];
    out[1] = led_color_palette_table[index][1];
    out[2] = led_color_palette_table[index][2];
}

bool led_effect_rgb(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t out[3])
{
    if (fx == FX_SOLID) {
        out[0] = out[1] = out[2] = brightness; // white
        return true;
    }
    if (fx == FX_SOLID_RED) {
        out[0] = brightness; // red
        out[1] = 0;
        out[2] = 0;
        return true;
    }
    if (fx >= FX_OFF) {
        return false;
    }
    led_color_wheel(led_effect_index(fx, row, col, phase), out);
    out[0] = (uint8_t)(((uint16_t)out[0] * brightness) >> 8);
    out[1] = (uint8_t)(((uint16_t)out[1] * brightness) >> 8);
    out[2] = (uint8_t)(((uint16_t)out[2] * brightness) >> 8);
    return true;
}

bool led_effect_rgb_colored(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t color, uint8_t out[3])
{
    if (fx == FX_SOLID) {
        out[0] = out[1] = out[2] = brightness; // white stays white
        return true;
    }
    if (fx == FX_SOLID_RED) {
        // Colourable solid: the palette colour at full brightness must stay
        // exact, so round instead of truncating (>>8 turns 255 into 254).
        led_color_palette(color, out);
        out[0] = (uint8_t)(((uint16_t)out[0] * brightness + 255) >> 8);
        out[1] = (uint8_t)(((uint16_t)out[1] * brightness + 255) >> 8);
        out[2] = (uint8_t)(((uint16_t)out[2] * brightness + 255) >> 8);
        return true;
    }
    if (fx >= FX_OFF) {
        return false;
    }
    // Animated effects keep their rainbow; the palette slot shifts its phase.
    led_color_wheel((uint8_t)(led_effect_index(fx, row, col, phase) + color * (256 / LED_COLOR_COUNT)), out);
    out[0] = (uint8_t)(((uint16_t)out[0] * brightness) >> 8);
    out[1] = (uint8_t)(((uint16_t)out[1] * brightness) >> 8);
    out[2] = (uint8_t)(((uint16_t)out[2] * brightness) >> 8);
    return true;
}

bool led_effect_mono(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t *out)
{
    if (fx == FX_SOLID) {
        *out = 255; // static full brightness
        return true;
    }
    if (fx == FX_SOLID_RED) {
        *out = 255; // static full brightness
        return true;
    }
    if (fx >= FX_OFF) {
        return false;
    }
    uint8_t x = led_effect_index(fx, row, col, phase);
    *out      = (x < 128) ? (uint8_t)(x << 1) : (uint8_t)((uint8_t)(255 - x) << 1);
    return true;
}
