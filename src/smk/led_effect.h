#pragma once

#include <stdint.h>

typedef enum {
    FX_RADIAL = 0, // rings radiating from the centre
    FX_HORIZONTAL, // wave across columns
    FX_VERTICAL,   // wave across rows
    FX_SOLID,      // static solid (colourable: Fn+. cycles white + palette)
    FX_BREATHING,  // palette colour pulsing in and out
    FX_RAINBOW,    // whole board cycling through the colour wheel
    FX_SNAKE,      // bright band sweeping along the columns
    FX_KNIGHT,     // bright segment bouncing left <-> right
    FX_GRADIENT,   // static colour ramp across the board
    FX_TWINKLE,    // pseudo-random per-key flicker
    FX_COUNT
} led_effect_t;

#define FX_OFF FX_COUNT

// Number of predefined colours the colour key cycles through.
#define LED_COLOR_COUNT 10

uint8_t led_effect_index(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase);

void led_color_wheel(uint8_t index, uint8_t out[3]);

// `index` is a palette slot (taken modulo LED_COLOR_COUNT).
void led_color_palette(uint8_t index, uint8_t out[3]);

bool led_effect_rgb(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t out[3]);

// As led_effect_rgb, but `color` selects one of LED_COLOR_COUNT palette colours:
// the FX_SOLID slot renders it as a static solid, and the animated effects use
// it to shift the rainbow's phase.
bool led_effect_rgb_colored(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t color, uint8_t out[3]);

bool led_effect_mono(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t *out);
