#pragma once

#include "sfr.h"

// The port argument may be a literal (0..7) or a keyboard-provided port-number
// macro, so expand it in a wrapper before pasting it into the SFR name.
#define GPIO_OUTPUT(port, mask) GPIO_OUTPUT_(port, mask)
#define GPIO_OUTPUT_(port, mask)        \
    do {                                \
        P##port##CR |= (uint8_t)(mask); \
    } while (0)
#define GPIO_INPUT(port, mask) GPIO_INPUT_(port, mask)
#define GPIO_INPUT_(port, mask)          \
    do {                                 \
        P##port##CR &= (uint8_t)~(mask); \
    } while (0)
#define GPIO_DIR_WRITE(port, value) GPIO_DIR_WRITE_(port, value)
#define GPIO_DIR_WRITE_(port, value)    \
    do {                                \
        P##port##CR = (uint8_t)(value); \
    } while (0)

#define GPIO_PULLUP_ON(port, mask) GPIO_PULLUP_ON_(port, mask)
#define GPIO_PULLUP_ON_(port, mask)      \
    do {                                 \
        P##port##PCR |= (uint8_t)(mask); \
    } while (0)
#define GPIO_PULLUP_OFF(port, mask) GPIO_PULLUP_OFF_(port, mask)
#define GPIO_PULLUP_OFF_(port, mask)      \
    do {                                  \
        P##port##PCR &= (uint8_t)~(mask); \
    } while (0)
#define GPIO_PULLUP_WRITE(port, value) GPIO_PULLUP_WRITE_(port, value)
#define GPIO_PULLUP_WRITE_(port, value)  \
    do {                                 \
        P##port##PCR = (uint8_t)(value); \
    } while (0)

#define GPIO_HIGH(port, mask) GPIO_HIGH_(port, mask)
#define GPIO_HIGH_(port, mask)      \
    do {                            \
        P##port |= (uint8_t)(mask); \
    } while (0)
#define GPIO_LOW(port, mask) GPIO_LOW_(port, mask)
#define GPIO_LOW_(port, mask)        \
    do {                             \
        P##port &= (uint8_t)~(mask); \
    } while (0)
#define GPIO_WRITE(port, value) GPIO_WRITE_(port, value)
#define GPIO_WRITE_(port, value)    \
    do {                            \
        P##port = (uint8_t)(value); \
    } while (0)
