#include "dynamic_keymap.h"

#ifdef VIAL_ENABLE

#    include "flash.h"
#    include "keycodes.h"
#    include "kbdef.h"
#    include "layout.h"
#    include "vial.h"

#    include <stdbool.h>
#    include <stdint.h>

// Pull in the generated counts without defining the definition arrays (vial.c
// owns those); this translation unit only needs VIAL_NUM_KEYS.
#    ifdef VIAL_DEFINITION_HEADER
#        define VIAL_DEFINITION_EXTERN 1
#        include VIAL_DEFINITION_HEADER
#    else
#        error "vial builds need VIAL_DEFINITION_HEADER (meson vial_def_gen.py)"
#    endif

#    define VIAL_KEYMAP_MAGIC0      0x56u // 'V'
#    define VIAL_KEYMAP_MAGIC1      0x4Bu // 'K'
#    define VIAL_KEYMAP_VERSION     1u
#    define VIAL_KEYMAP_SECTOR_SIZE FLASH_CFG_SIZE
#    define VIAL_KEYMAP_HEADER_SIZE 16u
#    define VIAL_KEYMAP_PAYLOAD     ((uint16_t)VIAL_LAYERS * VIAL_NUM_KEYS * 2u)
#    define VIAL_KEYMAP_TOTAL       (VIAL_KEYMAP_HEADER_SIZE + VIAL_KEYMAP_PAYLOAD)

// Header field offsets.
#    define KM_MAGIC0   0u
#    define KM_MAGIC1   1u
#    define KM_VERSION  2u
#    define KM_LAYERS   3u
#    define KM_HASH     4u
#    define KM_BASE     5u
#    define KM_TERM_LO  6u
#    define KM_TERM_HI  7u
#    define KM_FLAGS    8u
#    define KM_SEQ      9u
#    define KM_CKSUM_LO 10u
#    define KM_CKSUM_HI 11u
#    define KM_PAYLOAD  VIAL_KEYMAP_HEADER_SIZE

#    define KM_FLAG_PERMISSIVE_HOLD 0x01u
#    define KM_FLAG_HOLD_ON_OTHER   0x02u

#    define KM_TAPPING_TERM_DEFAULT 200u
#    define KM_TAPPING_TERM_MAX     10000u
#    define KM_HOLD_FLAGS_DEFAULT   KM_FLAG_PERMISSIVE_HOLD

_Static_assert(VIAL_LAYERS >= 1 && VIAL_LAYERS <= 16, "vial layer count must be 1..16");
_Static_assert(VIAL_KEYMAP_TOTAL <= VIAL_KEYMAP_SECTOR_SIZE, "vial keymap store does not fit one flash sector");

// Staging buffer for a whole sector. Only touched during init and writes; reads
// go straight to flash.
static __xdata uint8_t keymap_sector[VIAL_KEYMAP_SECTOR_SIZE];

static uint8_t  active_sector;
static bool     store_ready;
static uint16_t tapping_term;
static uint8_t  hold_flags;

static uint16_t sector_addr(uint8_t idx)
{
    return (uint16_t)(VIAL_KEYMAP_ADDR + (uint16_t)idx * VIAL_KEYMAP_SECTOR_SIZE);
}

// flash_read_into/program_from take a uint8_t length, so split long transfers.
static void keymap_read(uint16_t addr, __xdata uint8_t *dst, uint16_t len)
{
    while (len) {
        uint8_t chunk = (len > 0xFFu) ? 0xFFu : (uint8_t)len;
        flash_read_into(FLASH_CODE, addr, dst, chunk);
        addr = (uint16_t)(addr + chunk);
        dst += chunk;
        len = (uint16_t)(len - chunk);
    }
}

static void keymap_program(uint16_t addr, const __xdata uint8_t *src, uint16_t len)
{
    while (len) {
        uint8_t chunk = (len > 0xFFu) ? 0xFFu : (uint8_t)len;
        flash_program_from(FLASH_CODE, addr, src, chunk);
        addr = (uint16_t)(addr + chunk);
        src += chunk;
        len = (uint16_t)(len - chunk);
    }
}

static uint16_t sector_checksum(const __xdata uint8_t *buf)
{
    uint16_t sum = 0;
    for (uint16_t i = 0; i < VIAL_KEYMAP_TOTAL; i++) {
        uint8_t b = (i == KM_CKSUM_LO || i == KM_CKSUM_HI) ? 0 : buf[i];
        sum += b;
    }
    return sum;
}

static bool sector_valid(const __xdata uint8_t *buf)
{
    if (buf[KM_MAGIC0] != VIAL_KEYMAP_MAGIC0 || buf[KM_MAGIC1] != VIAL_KEYMAP_MAGIC1) {
        return false;
    }
    if (buf[KM_VERSION] != VIAL_KEYMAP_VERSION || buf[KM_LAYERS] != VIAL_LAYERS) {
        return false;
    }
    if (buf[KM_HASH] != vial_key_table_hash()) {
        return false;
    }
    const uint16_t stored = (uint16_t)(buf[KM_CKSUM_LO] | ((uint16_t)buf[KM_CKSUM_HI] << 8));
    return sector_checksum(buf) == stored;
}

static int16_t payload_index(uint8_t layer, uint8_t row, uint8_t col)
{
    if (layer >= VIAL_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        return -1;
    }
    const uint8_t slot = vial_key_slot((uint8_t)(row * MATRIX_COLS + col));
    if (slot == 0xFFu || slot >= VIAL_NUM_KEYS) {
        return -1;
    }
    return (int16_t)((uint16_t)layer * VIAL_NUM_KEYS + slot);
}

static uint16_t payload_read(uint16_t idx)
{
    // The active keymap is kept in the keymap_sector staging buffer (RAM):
    // dynamic_keymap_init() loads it via store_begin(), seed_defaults() fills it,
    // and store_commit() edits it before writing. Reading keycodes from flash on
    // every key event was slow (each read ran with interrupts off), so resolve
    // them from RAM instead.
    const uint16_t off = (uint16_t)(KM_PAYLOAD + idx * 2u);
    return (uint16_t)(((uint16_t)keymap_sector[off] << 8) | keymap_sector[off + 1]);
}

// Copy the active sector into the staging buffer so a caller can edit one field
// and commit it.
static void store_begin(void)
{
    keymap_read(sector_addr(active_sector), keymap_sector, VIAL_KEYMAP_TOTAL);
}

static void store_commit(void)
{
    const uint8_t spare = active_sector ^ 1u;

    keymap_sector[KM_SEQ]++;
    const uint16_t checksum    = sector_checksum(keymap_sector);
    keymap_sector[KM_CKSUM_LO] = (uint8_t)(checksum & 0xFF);
    keymap_sector[KM_CKSUM_HI] = (uint8_t)(checksum >> 8);

    flash_erase(FLASH_CODE, sector_addr(spare));
    keymap_program(sector_addr(spare), keymap_sector, VIAL_KEYMAP_TOTAL);
    active_sector = spare;
}

static void store_write_settings(void)
{
    if (!store_ready) {
        return;
    }
    store_begin();
    keymap_sector[KM_TERM_LO] = (uint8_t)(tapping_term & 0xFF);
    keymap_sector[KM_TERM_HI] = (uint8_t)(tapping_term >> 8);
    keymap_sector[KM_FLAGS]   = hold_flags;
    store_commit();
}

static void seed_defaults(void)
{
    for (uint16_t i = 0; i < VIAL_KEYMAP_TOTAL; i++) {
        keymap_sector[i] = 0;
    }

    keymap_sector[KM_MAGIC0]  = VIAL_KEYMAP_MAGIC0;
    keymap_sector[KM_MAGIC1]  = VIAL_KEYMAP_MAGIC1;
    keymap_sector[KM_VERSION] = VIAL_KEYMAP_VERSION;
    keymap_sector[KM_LAYERS]  = VIAL_LAYERS;
    keymap_sector[KM_HASH]    = vial_key_table_hash();
    keymap_sector[KM_BASE]    = 0;
    keymap_sector[KM_TERM_LO] = (uint8_t)(KM_TAPPING_TERM_DEFAULT & 0xFF);
    keymap_sector[KM_TERM_HI] = (uint8_t)(KM_TAPPING_TERM_DEFAULT >> 8);
    keymap_sector[KM_FLAGS]   = KM_HOLD_FLAGS_DEFAULT;
    keymap_sector[KM_SEQ]     = 0;

    for (uint8_t layer = 0; layer < VIAL_LAYERS; layer++) {
        for (uint8_t pos = 0; pos < MATRIX_ROWS * MATRIX_COLS; pos++) {
            const uint8_t slot = vial_key_slot(pos);
            if (slot == 0xFFu || slot >= VIAL_NUM_KEYS) {
                continue;
            }
            const uint8_t  row                        = (uint8_t)(pos / MATRIX_COLS);
            const uint8_t  col                        = (uint8_t)(pos % MATRIX_COLS);
            const uint16_t kc                         = (layer < KEYMAP_LAYERS) ? keymaps[layer][row][col] : KC_TRANSPARENT;
            const uint16_t idx                        = (uint16_t)layer * VIAL_NUM_KEYS + slot;
            keymap_sector[KM_PAYLOAD + idx * 2u]      = (uint8_t)(kc >> 8);
            keymap_sector[KM_PAYLOAD + idx * 2u + 1u] = (uint8_t)(kc & 0xFF);
        }
    }

    tapping_term  = KM_TAPPING_TERM_DEFAULT;
    hold_flags    = KM_HOLD_FLAGS_DEFAULT;
    active_sector = 1; // store_commit() writes the spare, so sector 0
    store_commit();
    set_default_layer(0);
    store_ready = true;
}

void dynamic_keymap_init(void)
{
    store_ready   = false;
    active_sector = 0;

    bool    valid[2];
    uint8_t seq[2];
    for (uint8_t i = 0; i < 2; i++) {
        keymap_read(sector_addr(i), keymap_sector, VIAL_KEYMAP_TOTAL);
        valid[i] = sector_valid(keymap_sector);
        seq[i]   = keymap_sector[KM_SEQ];
    }

    int8_t pick = -1;
    if (valid[0] && valid[1]) {
        // Newest sequence wins; the signed difference survives the 8-bit wrap.
        pick = ((int8_t)(seq[1] - seq[0]) > 0) ? 1 : 0;
    } else if (valid[0]) {
        pick = 0;
    } else if (valid[1]) {
        pick = 1;
    }

    if (pick < 0) {
        seed_defaults();
        return;
    }

    active_sector = (uint8_t)pick;
    store_begin();
    const uint8_t base = keymap_sector[KM_BASE];
    set_default_layer(base < VIAL_LAYERS ? base : 0);
    tapping_term = (uint16_t)(keymap_sector[KM_TERM_LO] | ((uint16_t)keymap_sector[KM_TERM_HI] << 8));
    hold_flags   = keymap_sector[KM_FLAGS];
    store_ready  = true;
}

uint16_t dynamic_keymap_get(uint8_t layer, uint8_t row, uint8_t col)
{
    const int16_t idx = payload_index(layer, row, col);
    if (idx < 0) {
        return KC_NO;
    }
    return payload_read((uint16_t)idx);
}

uint16_t dynamic_keymap_get_offset(uint16_t offset)
{
    const uint16_t index = (uint16_t)(offset / 2u);
    const uint16_t total = (uint16_t)VIAL_LAYERS * MATRIX_ROWS * MATRIX_COLS;
    if (index >= total) {
        return KC_NO;
    }
    const uint8_t layer = (uint8_t)(index / (MATRIX_ROWS * MATRIX_COLS));
    const uint8_t pos   = (uint8_t)(index % (MATRIX_ROWS * MATRIX_COLS));
    const uint8_t slot  = vial_key_slot(pos);
    if (slot == 0xFFu || slot >= VIAL_NUM_KEYS) {
        return KC_NO;
    }
    return payload_read((uint16_t)((uint16_t)layer * VIAL_NUM_KEYS + slot));
}

void dynamic_keymap_set_offset(uint16_t offset, const uint8_t *keycodes_be, uint8_t size)
{
    if (!store_ready || size < 2) {
        return;
    }
    const uint16_t total = (uint16_t)VIAL_LAYERS * MATRIX_ROWS * MATRIX_COLS;
    const uint16_t base  = (uint16_t)(offset / 2u);
    const uint8_t  count = (uint8_t)(size / 2u);

    store_begin();
    for (uint8_t i = 0; i < count; i++) {
        const uint16_t via_index = (uint16_t)(base + i);
        if (via_index >= total) {
            break;
        }
        const uint8_t layer = (uint8_t)(via_index / (MATRIX_ROWS * MATRIX_COLS));
        const uint8_t pos   = (uint8_t)(via_index % (MATRIX_ROWS * MATRIX_COLS));
        const uint8_t slot  = vial_key_slot(pos);
        if (slot == 0xFFu || slot >= VIAL_NUM_KEYS) {
            continue;
        }
        const uint16_t idx                        = (uint16_t)((uint16_t)layer * VIAL_NUM_KEYS + slot);
        keymap_sector[KM_PAYLOAD + idx * 2u]      = keycodes_be[i * 2u];
        keymap_sector[KM_PAYLOAD + idx * 2u + 1u] = keycodes_be[i * 2u + 1u];
    }
    store_commit();
}

void dynamic_keymap_set_keycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode)
{
    if (!store_ready) {
        return;
    }
    const int16_t idx = payload_index(layer, row, col);
    if (idx < 0) {
        return;
    }
    store_begin();
    keymap_sector[KM_PAYLOAD + (uint16_t)idx * 2u]      = (uint8_t)(keycode >> 8);
    keymap_sector[KM_PAYLOAD + (uint16_t)idx * 2u + 1u] = (uint8_t)(keycode & 0xFF);
    store_commit();
}

void dynamic_keymap_reset_keycode(uint8_t layer, uint8_t row, uint8_t col)
{
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        return;
    }
    const uint16_t kc = (layer < KEYMAP_LAYERS) ? keymaps[layer][row][col] : KC_TRANSPARENT;
    dynamic_keymap_set_keycode(layer, row, col, kc);
}

void dynamic_keymap_save_base_layer(uint8_t layer)
{
    if (!store_ready) {
        return;
    }
    store_begin();
    keymap_sector[KM_BASE] = layer;
    store_commit();
}

uint16_t dynamic_keymap_tapping_term(void)
{
    return tapping_term;
}

void dynamic_keymap_set_tapping_term(uint16_t term)
{
    if (term > KM_TAPPING_TERM_MAX) {
        term = KM_TAPPING_TERM_MAX;
    }
    if (term == tapping_term) {
        return;
    }
    tapping_term = term;
    store_write_settings();
}

bool dynamic_keymap_permissive_hold(void)
{
    return (hold_flags & KM_FLAG_PERMISSIVE_HOLD) != 0;
}

void dynamic_keymap_set_permissive_hold(bool on)
{
    const uint8_t flags = on ? (uint8_t)(hold_flags | KM_FLAG_PERMISSIVE_HOLD) : (uint8_t)(hold_flags & ~KM_FLAG_PERMISSIVE_HOLD);
    if (flags == hold_flags) {
        return;
    }
    hold_flags = flags;
    store_write_settings();
}

bool dynamic_keymap_hold_on_other_key_press(void)
{
    return (hold_flags & KM_FLAG_HOLD_ON_OTHER) != 0;
}

void dynamic_keymap_set_hold_on_other_key_press(bool on)
{
    const uint8_t flags = on ? (uint8_t)(hold_flags | KM_FLAG_HOLD_ON_OTHER) : (uint8_t)(hold_flags & ~KM_FLAG_HOLD_ON_OTHER);
    if (flags == hold_flags) {
        return;
    }
    hold_flags = flags;
    store_write_settings();
}

void dynamic_keymap_reset_settings(void)
{
    tapping_term = KM_TAPPING_TERM_DEFAULT;
    hold_flags   = KM_HOLD_FLAGS_DEFAULT;
    store_write_settings();
}

#endif // VIAL_ENABLE
