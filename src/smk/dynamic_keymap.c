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

// --- dynamic-entry store (Vial combos, tap dance, key overrides) ----------
//
// A separate A/B flash sector pair below the keymap store holds the dynamic
// entry tables: a small header (magic, version, counts, sequence, checksum)
// followed by the combo, tap-dance and key-override entries, 10 bytes each.
// Writes stage a new copy into the spare sector and switch to it, exactly like
// the keymap store.
#    if (VIAL_COMBO_ENTRIES + VIAL_TAP_DANCE_ENTRIES + VIAL_KEY_OVERRIDE_ENTRIES) > 0
#        ifndef VIAL_ENTRY_ADDR
#            error "vial dynamic entries need VIAL_ENTRY_ADDR (set vial_entry_sectors in meson)"
#        endif

#        define VIAL_ENTRY_MAGIC0            0x45u // 'E'
#        define VIAL_ENTRY_MAGIC1            0x4Eu // 'N'
#        define VIAL_ENTRY_VERSION           1u
#        define VIAL_ENTRY_HEADER_SIZE       16u
#        define VIAL_COMBO_ENTRY_SIZE        10u
#        define VIAL_TAP_DANCE_ENTRY_SIZE    10u
#        define VIAL_KEY_OVERRIDE_ENTRY_SIZE 10u
#        define VIAL_ENTRY_PAYLOAD           ((uint16_t)VIAL_COMBO_ENTRIES * VIAL_COMBO_ENTRY_SIZE + (uint16_t)VIAL_TAP_DANCE_ENTRIES * VIAL_TAP_DANCE_ENTRY_SIZE + (uint16_t)VIAL_KEY_OVERRIDE_ENTRIES * VIAL_KEY_OVERRIDE_ENTRY_SIZE)
#        define VIAL_ENTRY_TOTAL             (VIAL_ENTRY_HEADER_SIZE + VIAL_ENTRY_PAYLOAD)

#        define EN_MAGIC0      0u
#        define EN_MAGIC1      1u
#        define EN_VERSION     2u
#        define EN_COMBO_COUNT 3u
#        define EN_TD_COUNT    4u
#        define EN_KO_COUNT    5u
#        define EN_SEQ         6u
#        define EN_CKSUM_LO    7u
#        define EN_CKSUM_HI    8u
#        define EN_PAYLOAD     VIAL_ENTRY_HEADER_SIZE

#        define EN_COMBO_OFF EN_PAYLOAD
#        define EN_TD_OFF    (EN_COMBO_OFF + VIAL_COMBO_ENTRIES * VIAL_COMBO_ENTRY_SIZE)
#        define EN_KO_OFF    (EN_TD_OFF + VIAL_TAP_DANCE_ENTRIES * VIAL_TAP_DANCE_ENTRY_SIZE)

_Static_assert(VIAL_ENTRY_TOTAL <= VIAL_KEYMAP_SECTOR_SIZE, "vial entry store does not fit one flash sector");
_Static_assert(VIAL_COMBO_ENTRY_SIZE == sizeof(vial_combo_entry_t), "vial combo entry size must match the wire format");
_Static_assert(VIAL_TAP_DANCE_ENTRY_SIZE == sizeof(vial_tap_dance_entry_t), "vial tap dance entry size must match the wire format");
_Static_assert(VIAL_KEY_OVERRIDE_ENTRY_SIZE == sizeof(vial_key_override_entry_t), "vial key override entry size must match the wire format");

static __xdata uint8_t entry_sector[VIAL_ENTRY_TOTAL];
static uint8_t         entry_active_sector;
static bool            entry_ready;

static uint16_t entry_sector_addr(uint8_t idx)
{
    return (uint16_t)(VIAL_ENTRY_ADDR + (uint16_t)idx * VIAL_KEYMAP_SECTOR_SIZE);
}

static uint16_t entry_checksum(const __xdata uint8_t *buf)
{
    uint16_t sum = 0;
    for (uint16_t i = 0; i < VIAL_ENTRY_TOTAL; i++) {
        uint8_t b = (i == EN_CKSUM_LO || i == EN_CKSUM_HI) ? 0 : buf[i];
        sum += b;
    }
    return sum;
}

static bool entry_valid(const __xdata uint8_t *buf)
{
    if (buf[EN_MAGIC0] != VIAL_ENTRY_MAGIC0 || buf[EN_MAGIC1] != VIAL_ENTRY_MAGIC1) {
        return false;
    }
    if (buf[EN_VERSION] != VIAL_ENTRY_VERSION) {
        return false;
    }
    if (buf[EN_COMBO_COUNT] != VIAL_COMBO_ENTRIES || buf[EN_TD_COUNT] != VIAL_TAP_DANCE_ENTRIES || buf[EN_KO_COUNT] != VIAL_KEY_OVERRIDE_ENTRIES) {
        return false;
    }
    const uint16_t stored = (uint16_t)(buf[EN_CKSUM_LO] | ((uint16_t)buf[EN_CKSUM_HI] << 8));
    return entry_checksum(buf) == stored;
}

static void entry_begin(void)
{
    keymap_read(entry_sector_addr(entry_active_sector), entry_sector, VIAL_ENTRY_TOTAL);
}

static void entry_commit(void)
{
    const uint8_t spare = entry_active_sector ^ 1u;

    entry_sector[EN_SEQ]++;
    const uint16_t checksum   = entry_checksum(entry_sector);
    entry_sector[EN_CKSUM_LO] = (uint8_t)(checksum & 0xFF);
    entry_sector[EN_CKSUM_HI] = (uint8_t)(checksum >> 8);

    flash_erase(FLASH_CODE, entry_sector_addr(spare));
    keymap_program(entry_sector_addr(spare), entry_sector, VIAL_ENTRY_TOTAL);
    entry_active_sector = spare;
}

static void entry_seed_defaults(void)
{
    for (uint16_t i = 0; i < VIAL_ENTRY_TOTAL; i++) {
        entry_sector[i] = 0;
    }
    entry_sector[EN_MAGIC0]      = VIAL_ENTRY_MAGIC0;
    entry_sector[EN_MAGIC1]      = VIAL_ENTRY_MAGIC1;
    entry_sector[EN_VERSION]     = VIAL_ENTRY_VERSION;
    entry_sector[EN_COMBO_COUNT] = VIAL_COMBO_ENTRIES;
    entry_sector[EN_TD_COUNT]    = VIAL_TAP_DANCE_ENTRIES;
    entry_sector[EN_KO_COUNT]    = VIAL_KEY_OVERRIDE_ENTRIES;
    entry_sector[EN_SEQ]         = 0;
    entry_active_sector          = 1; // entry_commit() writes the spare, so sector 0
    entry_commit();
    entry_ready = true;
}

static void dynamic_entry_init(void)
{
    entry_ready         = false;
    entry_active_sector = 0;

    bool    valid[2];
    uint8_t seq[2];
    for (uint8_t i = 0; i < 2; i++) {
        keymap_read(entry_sector_addr(i), entry_sector, VIAL_ENTRY_TOTAL);
        valid[i] = entry_valid(entry_sector);
        seq[i]   = entry_sector[EN_SEQ];
    }

    int8_t pick = -1;
    if (valid[0] && valid[1]) {
        pick = ((int8_t)(seq[1] - seq[0]) > 0) ? 1 : 0;
    } else if (valid[0]) {
        pick = 0;
    } else if (valid[1]) {
        pick = 1;
    }

    if (pick < 0) {
        entry_seed_defaults();
        return;
    }

    entry_active_sector = (uint8_t)pick;
    entry_begin();
    entry_ready = true;
}

// All entries are big-endian 16-bit fields (plus a trailing byte group for key
// overrides); these helpers keep the section readers/writers small.
static uint16_t entry_read16(uint16_t off)
{
    return (uint16_t)(((uint16_t)entry_sector[off] << 8) | entry_sector[off + 1]);
}

static void entry_write16(uint16_t off, uint16_t v)
{
    entry_sector[off]     = (uint8_t)(v >> 8);
    entry_sector[off + 1] = (uint8_t)(v & 0xFF);
}

static void combo_read(uint8_t index, vial_combo_entry_t *entry)
{
    const uint16_t off = (uint16_t)(EN_COMBO_OFF + (uint16_t)index * VIAL_COMBO_ENTRY_SIZE);
    for (uint8_t i = 0; i < 4; i++) {
        entry->input[i] = entry_read16((uint16_t)(off + i * 2));
    }
    entry->output = entry_read16((uint16_t)(off + 8));
}

static void combo_write(uint8_t index, const vial_combo_entry_t *entry)
{
    const uint16_t off = (uint16_t)(EN_COMBO_OFF + (uint16_t)index * VIAL_COMBO_ENTRY_SIZE);
    for (uint8_t i = 0; i < 4; i++) {
        entry_write16((uint16_t)(off + i * 2), entry->input[i]);
    }
    entry_write16((uint16_t)(off + 8), entry->output);
}

static void tap_dance_read(uint8_t index, vial_tap_dance_entry_t *entry)
{
    const uint16_t off         = (uint16_t)(EN_TD_OFF + (uint16_t)index * VIAL_TAP_DANCE_ENTRY_SIZE);
    entry->on_tap              = entry_read16((uint16_t)(off + 0));
    entry->on_hold             = entry_read16((uint16_t)(off + 2));
    entry->on_double_tap       = entry_read16((uint16_t)(off + 4));
    entry->on_tap_hold         = entry_read16((uint16_t)(off + 6));
    entry->custom_tapping_term = entry_read16((uint16_t)(off + 8));
}

static void tap_dance_write(uint8_t index, const vial_tap_dance_entry_t *entry)
{
    const uint16_t off = (uint16_t)(EN_TD_OFF + (uint16_t)index * VIAL_TAP_DANCE_ENTRY_SIZE);
    entry_write16((uint16_t)(off + 0), entry->on_tap);
    entry_write16((uint16_t)(off + 2), entry->on_hold);
    entry_write16((uint16_t)(off + 4), entry->on_double_tap);
    entry_write16((uint16_t)(off + 6), entry->on_tap_hold);
    entry_write16((uint16_t)(off + 8), entry->custom_tapping_term);
}

static void key_override_read(uint8_t index, vial_key_override_entry_t *entry)
{
    const uint16_t off       = (uint16_t)(EN_KO_OFF + (uint16_t)index * VIAL_KEY_OVERRIDE_ENTRY_SIZE);
    entry->trigger           = entry_read16((uint16_t)(off + 0));
    entry->replacement       = entry_read16((uint16_t)(off + 2));
    entry->layers            = entry_read16((uint16_t)(off + 4));
    entry->trigger_mods      = entry_sector[off + 6];
    entry->negative_mod_mask = entry_sector[off + 7];
    entry->suppressed_mods   = entry_sector[off + 8];
    entry->options           = entry_sector[off + 9];
}

static void key_override_write(uint8_t index, const vial_key_override_entry_t *entry)
{
    const uint16_t off = (uint16_t)(EN_KO_OFF + (uint16_t)index * VIAL_KEY_OVERRIDE_ENTRY_SIZE);
    entry_write16((uint16_t)(off + 0), entry->trigger);
    entry_write16((uint16_t)(off + 2), entry->replacement);
    entry_write16((uint16_t)(off + 4), entry->layers);
    entry_sector[off + 6] = entry->trigger_mods;
    entry_sector[off + 7] = entry->negative_mod_mask;
    entry_sector[off + 8] = entry->suppressed_mods;
    entry_sector[off + 9] = entry->options;
}

uint8_t dynamic_keymap_combo_count(void)
{
    return VIAL_COMBO_ENTRIES;
}

int dynamic_keymap_get_combo(uint8_t index, vial_combo_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_COMBO_ENTRIES) {
        return 1;
    }
    combo_read(index, entry);
    return 0;
}

int dynamic_keymap_set_combo(uint8_t index, const vial_combo_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_COMBO_ENTRIES) {
        return 1;
    }
    entry_begin();
    combo_write(index, entry);
    entry_commit();
    return 0;
}

uint8_t dynamic_keymap_tap_dance_count(void)
{
    return VIAL_TAP_DANCE_ENTRIES;
}

int dynamic_keymap_get_tap_dance(uint8_t index, vial_tap_dance_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_TAP_DANCE_ENTRIES) {
        return 1;
    }
    tap_dance_read(index, entry);
    return 0;
}

int dynamic_keymap_set_tap_dance(uint8_t index, const vial_tap_dance_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_TAP_DANCE_ENTRIES) {
        return 1;
    }
    entry_begin();
    tap_dance_write(index, entry);
    entry_commit();
    return 0;
}

uint8_t dynamic_keymap_key_override_count(void)
{
    return VIAL_KEY_OVERRIDE_ENTRIES;
}

int dynamic_keymap_get_key_override(uint8_t index, vial_key_override_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_KEY_OVERRIDE_ENTRIES) {
        return 1;
    }
    key_override_read(index, entry);
    return 0;
}

int dynamic_keymap_set_key_override(uint8_t index, const vial_key_override_entry_t *entry)
{
    if (!entry_ready || index >= VIAL_KEY_OVERRIDE_ENTRIES) {
        return 1;
    }
    entry_begin();
    key_override_write(index, entry);
    entry_commit();
    return 0;
}

#    else // no dynamic entries configured

static void dynamic_entry_init(void) {}

uint8_t dynamic_keymap_combo_count(void)
{
    return 0;
}

int dynamic_keymap_get_combo(uint8_t index, vial_combo_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

int dynamic_keymap_set_combo(uint8_t index, const vial_combo_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

uint8_t dynamic_keymap_tap_dance_count(void)
{
    return 0;
}

int dynamic_keymap_get_tap_dance(uint8_t index, vial_tap_dance_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

int dynamic_keymap_set_tap_dance(uint8_t index, const vial_tap_dance_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

uint8_t dynamic_keymap_key_override_count(void)
{
    return 0;
}

int dynamic_keymap_get_key_override(uint8_t index, vial_key_override_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

int dynamic_keymap_set_key_override(uint8_t index, const vial_key_override_entry_t *entry)
{
    (void)index;
    (void)entry;
    return 1;
}

#    endif // dynamic entries

// --- macro store ----------------------------------------------------------
//
// A second A/B flash sector pair holds the VIA macro buffer. Same header and
// A/B write scheme as the other stores; the whole buffer is kept in RAM so the
// macro player can walk it without flash reads.
#    if VIAL_MACRO_BUFFER_SIZE > 0
#        ifndef VIAL_MACRO_ADDR
#            error "vial macros need VIAL_MACRO_ADDR (set vial_macro_sectors in meson)"
#        endif

#        define VIAL_MACRO_MAGIC0      0x4Du // 'M'
#        define VIAL_MACRO_MAGIC1      0x41u // 'A'
#        define VIAL_MACRO_VERSION     1u
#        define VIAL_MACRO_HEADER_SIZE 16u
#        define VIAL_MACRO_TOTAL       (VIAL_MACRO_HEADER_SIZE + VIAL_MACRO_BUFFER_SIZE)

#        define MA_MAGIC0   0u
#        define MA_MAGIC1   1u
#        define MA_VERSION  2u
#        define MA_SEQ      3u
#        define MA_CKSUM_LO 4u
#        define MA_CKSUM_HI 5u
#        define MA_PAYLOAD  VIAL_MACRO_HEADER_SIZE

_Static_assert(VIAL_MACRO_TOTAL <= VIAL_KEYMAP_SECTOR_SIZE, "vial macro store does not fit one flash sector");

static __xdata uint8_t macro_sector[VIAL_MACRO_TOTAL];
static uint8_t         macro_active_sector;
static bool            macro_ready;

static uint16_t macro_sector_addr(uint8_t idx)
{
    return (uint16_t)(VIAL_MACRO_ADDR + (uint16_t)idx * VIAL_KEYMAP_SECTOR_SIZE);
}

static uint16_t macro_checksum(const __xdata uint8_t *buf)
{
    uint16_t sum = 0;
    for (uint16_t i = 0; i < VIAL_MACRO_TOTAL; i++) {
        uint8_t b = (i == MA_CKSUM_LO || i == MA_CKSUM_HI) ? 0 : buf[i];
        sum += b;
    }
    return sum;
}

static bool macro_valid(const __xdata uint8_t *buf)
{
    if (buf[MA_MAGIC0] != VIAL_MACRO_MAGIC0 || buf[MA_MAGIC1] != VIAL_MACRO_MAGIC1) {
        return false;
    }
    if (buf[MA_VERSION] != VIAL_MACRO_VERSION) {
        return false;
    }
    const uint16_t stored = (uint16_t)(buf[MA_CKSUM_LO] | ((uint16_t)buf[MA_CKSUM_HI] << 8));
    return macro_checksum(buf) == stored;
}

static void macro_begin(void)
{
    keymap_read(macro_sector_addr(macro_active_sector), macro_sector, VIAL_MACRO_TOTAL);
}

static void macro_commit(void)
{
    const uint8_t spare = macro_active_sector ^ 1u;

    macro_sector[MA_SEQ]++;
    const uint16_t checksum   = macro_checksum(macro_sector);
    macro_sector[MA_CKSUM_LO] = (uint8_t)(checksum & 0xFF);
    macro_sector[MA_CKSUM_HI] = (uint8_t)(checksum >> 8);

    flash_erase(FLASH_CODE, macro_sector_addr(spare));
    keymap_program(macro_sector_addr(spare), macro_sector, VIAL_MACRO_TOTAL);
    macro_active_sector = spare;
}

static void macro_seed_defaults(void)
{
    for (uint16_t i = 0; i < VIAL_MACRO_TOTAL; i++) {
        macro_sector[i] = 0;
    }
    macro_sector[MA_MAGIC0]  = VIAL_MACRO_MAGIC0;
    macro_sector[MA_MAGIC1]  = VIAL_MACRO_MAGIC1;
    macro_sector[MA_VERSION] = VIAL_MACRO_VERSION;
    macro_sector[MA_SEQ]     = 0;
    macro_active_sector      = 1; // macro_commit() writes the spare, so sector 0
    macro_commit();
    macro_ready = true;
}

static void dynamic_macro_init(void)
{
    macro_ready         = false;
    macro_active_sector = 0;

    bool    valid[2];
    uint8_t seq[2];
    for (uint8_t i = 0; i < 2; i++) {
        keymap_read(macro_sector_addr(i), macro_sector, VIAL_MACRO_TOTAL);
        valid[i] = macro_valid(macro_sector);
        seq[i]   = macro_sector[MA_SEQ];
    }

    int8_t pick = -1;
    if (valid[0] && valid[1]) {
        pick = ((int8_t)(seq[1] - seq[0]) > 0) ? 1 : 0;
    } else if (valid[0]) {
        pick = 0;
    } else if (valid[1]) {
        pick = 1;
    }

    if (pick < 0) {
        macro_seed_defaults();
        return;
    }

    macro_active_sector = (uint8_t)pick;
    macro_begin();
    macro_ready = true;
}

uint8_t dynamic_keymap_macro_count(void)
{
    return VIAL_MACRO_COUNT;
}

uint16_t dynamic_keymap_macro_buffer_size(void)
{
    return VIAL_MACRO_BUFFER_SIZE;
}

uint8_t dynamic_keymap_macro_read_byte(uint16_t offset)
{
    if (!macro_ready || offset >= VIAL_MACRO_BUFFER_SIZE) {
        return 0;
    }
    return macro_sector[MA_PAYLOAD + offset];
}

void dynamic_keymap_macro_get_buffer(uint16_t offset, uint16_t size, uint8_t *out)
{
    for (uint16_t i = 0; i < size; i++) {
        out[i] = dynamic_keymap_macro_read_byte((uint16_t)(offset + i));
    }
}

void dynamic_keymap_macro_set_buffer(uint16_t offset, uint16_t size, const uint8_t *in)
{
    if (!macro_ready) {
        return;
    }
    macro_begin();
    for (uint16_t i = 0; i < size; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        if (o >= VIAL_MACRO_BUFFER_SIZE) {
            break;
        }
        macro_sector[MA_PAYLOAD + o] = in[i];
    }
    macro_commit();
}

void dynamic_keymap_macro_reset(void)
{
    if (!macro_ready) {
        return;
    }
    macro_begin();
    for (uint16_t i = 0; i < VIAL_MACRO_BUFFER_SIZE; i++) {
        macro_sector[MA_PAYLOAD + i] = 0;
    }
    macro_commit();
}

#    else // VIAL_MACRO_BUFFER_SIZE == 0

static void dynamic_macro_init(void) {}

uint8_t dynamic_keymap_macro_count(void)
{
    return 0;
}

uint16_t dynamic_keymap_macro_buffer_size(void)
{
    return 0;
}

uint8_t dynamic_keymap_macro_read_byte(uint16_t offset)
{
    (void)offset;
    return 0;
}

void dynamic_keymap_macro_get_buffer(uint16_t offset, uint16_t size, uint8_t *out)
{
    (void)offset;
    (void)size;
    (void)out;
}

void dynamic_keymap_macro_set_buffer(uint16_t offset, uint16_t size, const uint8_t *in)
{
    (void)offset;
    (void)size;
    (void)in;
}

void dynamic_keymap_macro_reset(void) {}

#    endif // VIAL_MACRO_BUFFER_SIZE

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
    } else {
        active_sector = (uint8_t)pick;
        store_begin();
        const uint8_t base = keymap_sector[KM_BASE];
        set_default_layer(base < VIAL_LAYERS ? base : 0);
        tapping_term = (uint16_t)(keymap_sector[KM_TERM_LO] | ((uint16_t)keymap_sector[KM_TERM_HI] << 8));
        hold_flags   = keymap_sector[KM_FLAGS];
        store_ready  = true;
    }

    dynamic_entry_init();
    dynamic_macro_init();
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
