#include "kbdef.h"
#include "gpio.h"
#include "user_matrix.h"

// Columns span P6 (C0-C7), P5 (C8-C11) and P4 (C12/C13). They are configured
// as outputs at boot and stay output for the whole scan: the LED phase, which
// time-multiplexes these same pins with the key scan, arrives later and will
// add the scan_pre/scan_post direction handoff.

#define KB_C_P4_MASK (uint8_t)(KB_C12_P4_0 | KB_C13_P4_2)
#define KB_C_P5_MASK (uint8_t)(KB_C8_P5_0 | KB_C9_P5_1 | KB_C10_P5_2 | KB_C11_P5_7)
#define KB_C_P6_MASK (uint8_t)(KB_C0_P6_0 | KB_C1_P6_1 | KB_C2_P6_2 | KB_C3_P6_3 | KB_C4_P6_4 | KB_C5_P6_5 | KB_C6_P6_6 | KB_C7_P6_7)

void user_matrix_cols_deselect_all(void)
{
    GPIO_HIGH(4, KB_C_P4_MASK);
    GPIO_HIGH(5, KB_C_P5_MASK);
    GPIO_HIGH(6, KB_C_P6_MASK);
}

void user_matrix_col_select(uint8_t col) // active-low: drive LOW
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

void user_matrix_col_deselect(uint8_t col) // active-low: drive HIGH (idle)
{
    switch (col) {
        case 0:
            KB_C0 = 1;
            break;
        case 1:
            KB_C1 = 1;
            break;
        case 2:
            KB_C2 = 1;
            break;
        case 3:
            KB_C3 = 1;
            break;
        case 4:
            KB_C4 = 1;
            break;
        case 5:
            KB_C5 = 1;
            break;
        case 6:
            KB_C6 = 1;
            break;
        case 7:
            KB_C7 = 1;
            break;
        case 8:
            KB_C8 = 1;
            break;
        case 9:
            KB_C9 = 1;
            break;
        case 10:
            KB_C10 = 1;
            break;
        case 11:
            KB_C11 = 1;
            break;
        case 12:
            KB_C12 = 1;
            break;
        case 13:
            KB_C13 = 1;
            break;
    }
}

uint8_t user_matrix_read_rows(void)
{
    // R0-R2 = P7.1-3 land in bits 0-2, R3/R4 = P5.3/4 in bits 3/4; bits 5-7
    // are unused and read as idle-high.
    return (uint8_t)(((P7 >> 1) & 0x07) | (P5 & 0x18) | 0xE0);
}
