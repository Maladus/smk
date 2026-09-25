#pragma once

#include <stdint.h>
#include "report.h"

// Vial / VIA raw-HID config interface.
//
// The host (Vial desktop, Vial Web, vial.rocks) sends a 32-byte report over the
// raw-HID collection (usage page 0xFF60, usage 0x61) and reads a 32-byte reply.
// The transport (EP0 SET_REPORT reassembly, EP2 IN reply) lives in usb.c; this
// module is the protocol handler.

#ifdef VIAL_ENABLE
#    define VIAL_REPORT_SIZE RAW_HID_REPORT_SIZE

// Process one request (in) and fill the reply (out). Both are VIAL_REPORT_SIZE.
void vial_handle(const uint8_t *in, uint8_t *out);

// Key table accessors over the generated definition. `vial_key_slot()` returns
// the stored slot for a matrix position (row * cols + col), or 0xFF when the
// position holds no key; `vial_key_table_hash()` identifies the table so a
// changed layout invalidates a stale keymap store.
uint8_t vial_key_slot(uint8_t pos);
uint8_t vial_key_table_hash(void);
#endif
