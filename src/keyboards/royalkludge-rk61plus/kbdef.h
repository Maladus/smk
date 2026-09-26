#pragma once

#include "sh68f90.h"
#include "keycodes.h"
#include <stdint.h>
#include <stdbool.h>

#define MATRIX_ROWS 5
#define MATRIX_COLS 14

// Number of layers in the const keymap (layouts/default/layout.c). Vial seeds
// the dynamic store from these; layers past it start transparent. The Vial
// build adds the Fn+Shift layer (number-row media keys).
#ifdef VIAL_ENABLE
#    define KEYMAP_LAYERS 3
#else
#    define KEYMAP_LAYERS 2
#endif

// Layer indices, shared by the keymap (layouts/default/layout.c) and the layer
// engine (src/smk/matrix.c).
#define LAYER_BASE     0
#define LAYER_FN       1
#define LAYER_FN_SHIFT 2

// Row Pin Bits
#define KB_R0_P7_1 _P7_1
#define KB_R1_P7_2 _P7_2
#define KB_R2_P7_3 _P7_3
#define KB_R3_P5_3 _P5_3
#define KB_R4_P5_4 _P5_4

// Row Pins
#define KB_R0 P7_1
#define KB_R1 P7_2
#define KB_R2 P7_3
#define KB_R3 P5_3
#define KB_R4 P5_4

// Column Pin Bits
#define KB_C0_P6_0  _P6_0
#define KB_C1_P6_1  _P6_1
#define KB_C2_P6_2  _P6_2
#define KB_C3_P6_3  _P6_3
#define KB_C4_P6_4  _P6_4
#define KB_C5_P6_5  _P6_5
#define KB_C6_P6_6  _P6_6
#define KB_C7_P6_7  _P6_7
#define KB_C8_P5_0  _P5_0
#define KB_C9_P5_1  _P5_1
#define KB_C10_P5_2 _P5_2
#define KB_C11_P5_7 _P5_7
#define KB_C12_P4_0 _P4_0
#define KB_C13_P4_2 _P4_2

// Column Pins
#define KB_C0  P6_0
#define KB_C1  P6_1
#define KB_C2  P6_2
#define KB_C3  P6_3
#define KB_C4  P6_4
#define KB_C5  P6_5
#define KB_C6  P6_6
#define KB_C7  P6_7
#define KB_C8  P5_0
#define KB_C9  P5_1
#define KB_C10 P5_2
#define KB_C11 P5_7
#define KB_C12 P4_0
#define KB_C13 P4_2

// Board switches (read active-low; pull-ups enabled). Mapped on hardware:
//   P5.6 = B/G band switch: B (high) = BLE, G (low) = 2.4G
//   P5.5 = on/off switch:   on (low) = wireless, off (high) = wired/USB
// (The stock switch-poll trace in the plan had P5.5/P5.6 swapped.)
#define BAND_SWITCH  P5_6 // 1 = BLE (B), 0 = 2.4G (G)
#define POWER_SWITCH P5_5 // 0 = on (wireless), 1 = off (wired/USB)

#define BAND_SWITCH_P5_6  _P5_6
#define POWER_SWITCH_P5_5 _P5_5

// Enable group: hub enable + charge enable + 3 more. The stock firmware drives
// the five pins high at boot and low at park as one group; it never gates them
// individually.
#define ENABLE_P0_5 _P0_5
#define ENABLE_P4_3 _P4_3
#define ENABLE_P4_5 _P4_5
#define ENABLE_P4_6 _P4_6
#define ENABLE_P7_4 _P7_4

// RC battery measurement pins (RC-timing loop; the measure phase implements it).
#define BAT_RC_P0_0 _P0_0
#define BAT_RC_P0_1 _P0_1

// Status inputs.
#define STATUS_P0_6 _P0_6
#define STATUS_P0_7 _P0_7
#define STATUS_P7_5 _P7_5
#define STATUS_P7_6 _P7_6
#define STATUS_P7_7 _P7_7

// Output driven only while parked (stock clears it low at boot and park).
#define PARK_P7_0 _P7_0

// Boot values: stock `MOV P0,#24` / `MOV P4,#FD` / `MOV P7,#10` (#nn is hex).
#define BOOT_P0_VALUE 0x24
#define BOOT_P4_VALUE 0xFD
#define BOOT_P7_VALUE 0x10

// Park values, in the stock teardown write order (see user_sleep.c):
//   ANL P7,#EE  ANL P4,#92  ANL P0,#1F  ANL P7,#3F  ANL P0,#FC
#define PARK_P7_EE_MASK 0xEE
#define PARK_P4_92_MASK 0x92
#define PARK_P0_1F_MASK 0x1F
#define PARK_P7_3F_MASK 0x3F
#define PARK_P0_FC_MASK 0xFC

// BK3632 radio (same part as the nuphy-air60, wired to different pins).
#define RF_BB_SPI_SCK  P4_7
#define RF_BB_SPI_MOSI P0_4
#define RF_BB_SPI_MISO P0_3
#define RF_BB_SPI_CS   P4_4
#define RF_BB_WAKE     P0_2
#define RF_BB_ACK      P4_1
#define RF_BB_SPI_ACK  RF_BB_ACK

#define RF_BB_SPI_SCK_P4_7  _P4_7
#define RF_BB_SPI_MOSI_P0_4 _P0_4
#define RF_BB_SPI_MISO_P0_3 _P0_3
#define RF_BB_SPI_CS_P4_4   _P4_4
#define RF_BB_WAKE_P0_2     _P0_2
#define RF_BB_ACK_P4_1      _P4_1

// Port/mask descriptors consumed by the shared bb_spi.c bit-bang driver. The
// radio's WAKE line plays the role of the nuphy-air60 SPI MOT (drive low to
// wake the part before a transfer).
#define RF_BB_SPI_CS_PORT   4
#define RF_BB_SPI_CS_MASK   RF_BB_SPI_CS_P4_4
#define RF_BB_SPI_SCK_PORT  4
#define RF_BB_SPI_SCK_MASK  RF_BB_SPI_SCK_P4_7
#define RF_BB_SPI_MOSI_PORT 0
#define RF_BB_SPI_MOSI_MASK RF_BB_SPI_MOSI_P0_4
#define RF_BB_SPI_MOT       RF_BB_WAKE
#define RF_BB_SPI_MOT_PORT  0
#define RF_BB_SPI_MOT_MASK  RF_BB_WAKE_P0_2

// LED row/colour sink PWM channels. The RGB matrix is the transpose of the
// nuphy-air60: the PWM channels are the row/colour sinks and the key-matrix
// columns are the LED columns. Each keyboard row owns three consecutive
// channels in the order green, red, blue (see
// docs/keyboards/royalkludge-rk61plus.md). PWM20-22 (P3.0-2) are the spare,
// unconnected row and stay dark.
#define LED_SINK_R0G PWM23 // Esc row
#define LED_SINK_R0R PWM24
#define LED_SINK_R0B PWM25
#define LED_SINK_R1G PWM10 // Tab row
#define LED_SINK_R1R PWM11
#define LED_SINK_R1B PWM12
#define LED_SINK_R2G PWM13 // Caps row
#define LED_SINK_R2R PWM14
#define LED_SINK_R2B PWM15
#define LED_SINK_R3G PWM03 // Shift row
#define LED_SINK_R3R PWM04
#define LED_SINK_R3B PWM05
#define LED_SINK_R4G PWM00 // Ctrl row
#define LED_SINK_R4R PWM01
#define LED_SINK_R4B PWM02

// Custom keycodes. Fn layer Q/W/E select the BT channel (LNK_BT1/2/3).
enum custom_keycodes {
    LNK_BT1 = KB_KEYCODE_BASE,
    LNK_BT2,
    LNK_BT3,

    FX_PREV, // previous RGB effect
    FX_NEXT, // next RGB effect
    SPD_DN,  // animation speed down
    SPD_UP,  // animation speed up
    BRI_DN,  // backlight brightness down
    BRI_UP,  // backlight brightness up
    CLR_FWD, // cycle through the predefined colours

    KB_SAFE_RANGE,
};
