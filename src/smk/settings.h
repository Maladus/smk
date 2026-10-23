#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t led_effect;
    uint8_t led_brightness;
    uint8_t led_speed;
    uint8_t led_color;
    uint8_t ul_effect;
    uint8_t ul_brightness;
    uint8_t ul_speed;
    uint8_t battery_indicator_on;
    uint8_t rf_link;
    uint8_t combo_term; // ms; 0 keeps the compiled default (COMBO_TERM)
} user_settings_t;

extern user_settings_t user_settings;

bool settings_load(void);

void settings_save(void);

void settings_mark_dirty(void);

void settings_task(void);

void settings_save_pre(void);
void settings_save_post(void);

#if DEBUG == 1
void settings_dump(void);
#endif
