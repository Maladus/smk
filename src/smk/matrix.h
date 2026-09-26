#pragma once

#include <stdint.h>
#include <stdbool.h>

void    matrix_init();
uint8_t matrix_task();

// True when the top-left key (R0/C0) is held at power-on. Call before
// matrix_init() to hand a wedged image back to the ISP bootloader.
bool matrix_recovery_held(void);

void matrix_scan_full();

#ifdef VIAL_ENABLE
// Raw switch state for the Vial matrix tester: true when the key at (row, col)
// is held. Reads the scan-written matrix[col] bitmap.
bool matrix_is_on(uint8_t row, uint8_t col);

// Layer engine hooks used by the tapping engine.
void matrix_process_key(uint8_t row, uint8_t col, bool pressed);
void matrix_tap_keycode(uint16_t keycode);
void matrix_send_keycode(uint16_t keycode, bool pressed);
void matrix_layer_activate(uint8_t layer);
void matrix_layer_deactivate(uint8_t layer);
void matrix_layer_toggle(uint8_t layer);
#endif
