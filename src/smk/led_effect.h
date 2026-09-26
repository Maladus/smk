#pragma once

#include <stdint.h>

typedef enum {
    FX_RADIAL = 0, // rings radiating from the centre
    FX_HORIZONTAL, // wave across columns
    FX_VERTICAL,   // wave across rows
    FX_SOLID,      // static white (no animation)
    FX_SOLID_RED,  // static red (no animation)
    FX_COUNT
} led_effect_t;

#define FX_OFF FX_COUNT

uint8_t led_effect_index(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase);

void led_color_wheel(uint8_t index, uint8_t out[3]);

bool led_effect_rgb(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t out[3]);

// As led_effect_rgb, but with a colour-wheel offset folded into the index so a
// colour key (or the VIA lighting colour) shifts the whole animation round the
// wheel. The FX_SOLID_RED slot renders the wheel at the offset, i.e. it is a
// colourable solid (red at offset 0).
bool led_effect_rgb_colored(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t brightness, uint8_t color, uint8_t out[3]);

bool led_effect_mono(led_effect_t fx, uint8_t row, uint8_t col, uint8_t phase, uint8_t *out);
