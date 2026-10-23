// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Board identity: a persistent USB serial and a user-chosen name.
//
// Kept in the last two 4 KB sectors of the 16 MB flash die, far above the 2 MB
// QMK uses for firmware and settings, so firmware flashes, settings wipes and VIA
// resets never touch it; only a whole-chip erase does. The two sectors hold
// alternating copies with a sequence number and a CRC: a save goes to the older
// copy, so a power cut part-way through leaves the newer one intact.
//
// Writes go through the RP2040 boot ROM rather than the Pico SDK's flash
// wrappers, which refuse addresses above PICO_FLASH_SIZE_BYTES (2 MB here);
// raising that would move the wear-levelling area. Before touching the region
// the flash's JEDEC capacity is checked: on a smaller die these addresses would
// wrap onto the firmware or the settings.
//
// The serial is derived once and then stored: the flash die's unique ID when it
// reports one, otherwise 64 random bits. A stored serial never changes, so a
// browser's WebHID grant survives every update.

#include "svalboard.h"
#include "via.h"
#include "client_wrapper.h"
#include "util.h"
#include "identity.h"
#include "hardware_id.h"
#include "timer.h"
#include "wait.h"
#include <string.h>

#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/rosc.h"

_Static_assert(IDENTITY_SECTOR_A == 0x1000000u - 2 * FLASH_SECTOR_SIZE, "identity sector A is second-to-last");
_Static_assert(IDENTITY_SECTOR_B == 0x1000000u - 1 * FLASH_SECTOR_SIZE, "identity sector B is last");
#define IDENTITY_MAGIC 0x44495653u // "SVID"
#define IDENTITY_VERSION 1
#define JEDEC_CAPACITY_16MB 0x18    // 2^24 bytes
#define ERASE_BLOCK_SIZE (1u << 16) // as the SDK passes to the ROM: 64 KB block erase where aligned,
#define ERASE_BLOCK_CMD 0xD8        // otherwise 4 KB sector erases

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length; // sizeof(identity_record_t), for forward compatibility
    uint32_t seq;
    uint8_t  serial[IDENTITY_SERIAL_BYTES];
    uint8_t  serial_source; // identity_serial_source_t
    uint8_t  name_len;      // bytes of UTF-8 in name, no terminator
    uint8_t  flags;         // reserved; see identity.h
    uint8_t  reserved;
    char     name[IDENTITY_NAME_MAX_BYTES];
    uint32_t crc;
} identity_record_t;
_Static_assert(sizeof(identity_record_t) <= FLASH_PAGE_SIZE, "identity record fits one flash page");

static identity_record_t current;
static bool              loaded;
static bool              region_ok; // the die is 16 MB, so the region exists
static char              name_z[IDENTITY_NAME_MAX_BYTES + 1];

// ---- flash access (runs from RAM: XIP is off while the die is busy) ---------

extern const uint8_t BOOT2_ROM[256];
static uint32_t      boot2_copy[64];
static uint8_t       page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static void __no_inline_not_in_flash_func(identity_flash_write)(uint32_t offset, const uint8_t *page) {
    rom_connect_internal_flash_fn connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn    program  = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn      flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);

    __compiler_memory_barrier();
    connect();
    exit_xip();
    erase(offset, FLASH_SECTOR_SIZE, ERASE_BLOCK_SIZE, ERASE_BLOCK_CMD); // one 4 KB sector
    program(offset, page, FLASH_PAGE_SIZE);
    flush();
    ((void (*)(void))((intptr_t)boot2_copy + 1))(); // back to XIP via boot2
}

static uint8_t jedec_capacity(void) {
    uint8_t  tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, sizeof(tx));
    restore_interrupts(irq);
    return rx[3];
}

// ---- records ------------------------------------------------------------------

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static bool record_valid(const identity_record_t *r) {
    return r->magic == IDENTITY_MAGIC && r->version == IDENTITY_VERSION && r->length == sizeof(*r) && r->name_len <= IDENTITY_NAME_MAX_BYTES && r->crc == crc32((const uint8_t *)r, offsetof(identity_record_t, crc));
}

static const identity_record_t *record_at(uint32_t offset) {
    return (const identity_record_t *)(XIP_NOCACHE_NOALLOC_BASE + offset);
}

static bool save(void) {
    if (!region_ok) return false;
    const identity_record_t *a = record_at(IDENTITY_SECTOR_A), *b = record_at(IDENTITY_SECTOR_B);
    bool                     a_ok = record_valid(a), b_ok = record_valid(b);
    uint32_t                 newest = 0;
    if (a_ok) newest = a->seq;
    if (b_ok && b->seq > newest) newest = b->seq;
    // Write over the older copy (or A when neither is valid).
    uint32_t target = (a_ok && (!b_ok || a->seq >= b->seq)) ? IDENTITY_SECTOR_B : IDENTITY_SECTOR_A;

    current.magic   = IDENTITY_MAGIC;
    current.version = IDENTITY_VERSION;
    current.length  = sizeof(current);
    current.seq     = newest + 1;
    current.crc     = crc32((const uint8_t *)&current, offsetof(identity_record_t, crc));
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &current, sizeof(current));

    uint32_t irq = save_and_disable_interrupts();
    identity_flash_write(target, page_buf);
    restore_interrupts(irq);
    return record_valid(record_at(target)) && record_at(target)->seq == current.seq;
}

static void derive_serial(void) {
    uint8_t  id[IDENTITY_SERIAL_BYTES];
    uint32_t irq = save_and_disable_interrupts();
    flash_get_unique_id(id);
    restore_interrupts(irq);
    // Dies that ignore the unique-ID command read back all zeros or all ones.
    bool all00 = true, allFF = true;
    for (uint8_t i = 0; i < sizeof(id); i++) {
        all00 &= id[i] == 0x00;
        allFF &= id[i] == 0xFF;
    }
    if (!all00 && !allFF) {
        memcpy(current.serial, id, sizeof(id));
        current.serial_source = IDENTITY_SERIAL_FLASH_ID;
        return;
    }
    // No usable die ID: 64 bits from the ring oscillator, a bit per read, mixed
    // with the timer so two boards started together still differ.
    for (uint8_t i = 0; i < sizeof(id); i++) {
        uint8_t v = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            v = (v << 1) | (rosc_hw->randombit & 1);
            wait_us(1);
        }
        current.serial[i] = v ^ (uint8_t)(timer_read32() >> (i % 4) * 8);
    }
    current.serial_source = IDENTITY_SERIAL_RANDOM;
}

void identity_init(void) {
    if (loaded) return;
    loaded = true;
    memcpy(boot2_copy, BOOT2_ROM, sizeof(BOOT2_ROM));
    region_ok = jedec_capacity() == JEDEC_CAPACITY_16MB;

    if (region_ok) {
        const identity_record_t *a = record_at(IDENTITY_SECTOR_A), *b = record_at(IDENTITY_SECTOR_B);
        bool                     a_ok = record_valid(a), b_ok = record_valid(b);
        if (a_ok || b_ok) {
            memcpy(&current, (a_ok && (!b_ok || a->seq >= b->seq)) ? a : b, sizeof(current));
        }
    }
    if (current.magic != IDENTITY_MAGIC) {
        memset(&current, 0, sizeof(current));
        derive_serial();
        save(); // first boot: pin the serial (no-op where the region is missing)
    }
    memcpy(name_z, current.name, current.name_len);
    name_z[current.name_len] = 0;
}

// ---- public API ------------------------------------------------------------------

#include "build_reset.h"
bool identity_build_record_write(const void *data, uint32_t len) {
    identity_init();
    if (!region_ok || len > sizeof(page_buf)) return false;
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, data, len);
    uint32_t irq = save_and_disable_interrupts();
    identity_flash_write(BUILD_RESET_SECTOR, page_buf);
    restore_interrupts(irq);
    return memcmp((const void *)(XIP_NOCACHE_NOALLOC_BASE + BUILD_RESET_SECTOR), data, len) == 0;
}

#ifdef SVAL_EITHER_HAND
#    include "side.h"
bool identity_side_record_write(uint32_t sector, const void *data, uint32_t len) {
    identity_init();
    if (!region_ok || len > sizeof(page_buf) || (sector != SVAL_SIDE_SECTOR_A && sector != SVAL_SIDE_SECTOR_B)) return false;
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, data, len);
    uint32_t irq = save_and_disable_interrupts();
    identity_flash_write(sector, page_buf);
    restore_interrupts(irq);
    return memcmp((const void *)(XIP_NOCACHE_NOALLOC_BASE + sector), data, len) == 0;
}
#endif

bool identity_available(void) {
    identity_init();
    return region_ok;
}

const uint8_t *identity_serial(identity_serial_source_t *source) {
    identity_init();
    if (source) *source = current.serial_source;
    return current.serial;
}

const char *identity_name(void) {
    identity_init();
    return name_z;
}

static bool utf8_valid(const uint8_t *s, uint8_t len) {
    for (uint8_t i = 0; i < len;) {
        uint8_t c = s[i], extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0xFF;
        if (extra == 0xFF || c == 0 || (c < 0x20 && c != '\t')) return false;
        if (i + extra >= len) return false; // sequence runs past the end
        for (uint8_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

identity_status_t identity_set_name(const char *name, uint8_t len) {
    identity_init();
    if (!region_ok) return IDENTITY_UNAVAILABLE;
    if (len > IDENTITY_NAME_MAX_BYTES || !utf8_valid((const uint8_t *)name, len)) return IDENTITY_INVALID;
    if (len == current.name_len && memcmp(name, current.name, len) == 0) return IDENTITY_OK;
    memset(current.name, 0, sizeof(current.name));
    memcpy(current.name, name, len);
    current.name_len = len;
    if (!save()) return IDENTITY_WRITE_FAILED;
    memcpy(name_z, current.name, len);
    name_z[len] = 0;
    return IDENTITY_OK;
}

// ---- hooks into QMK -----------------------------------------------------------------

// The USB serial: QMK formats get_hardware_id() as hex after SERIAL_NUMBER_PREFIX.
hardware_id_t get_hardware_id(void) {
    hardware_id_t id = {0};
    memcpy(&id, identity_serial(NULL), IDENTITY_SERIAL_BYTES);
    return id;
}

// The USB product string: the user's name, or the compiled PRODUCT when unset.
const char *usb_product_string(void) {
    const char *name = identity_name();
    return name[0] ? name : NULL;
}

// ---- host protocol: VIA custom values on channel IDENTITY_CHANNEL --------------
//
// Request/response layout (VIA echoes the packet with value_data rewritten):
//   [0] id_custom_get_value / id_custom_set_value  [1] channel 0x49  [2] op  [3..] data
// get op 0 INFO   -> [3] protocol 1, [4] available, [5] name_len, [6] name max bytes,
//                    [7] serial source, [8..15] serial
// get op 1 NAME   req [3] offset -> [3] status 0, [4] count, [5..] bytes
// set op 1 STAGE  req [3] offset, [4] count, [5..] bytes  -> [3] status
// A name chunk is at most the report less 5 bytes: 59 in a bare 64-byte report
// and 53 when the host sends VIA inside the client wrapper, which costs 6 bytes
// of the reply (27 and 21 with 32-byte reports).
// set op 2 COMMIT req [3] total length -> [3] identity_status_t
// set op 3 REBOOT -> [3] 0; reboots ~100 ms later so the new name enumerates
// Status 0 = OK, 1 = bad request / invalid, 2 = unavailable, 3 = write failed.

static char     staged[IDENTITY_NAME_MAX_BYTES];
static uint32_t reboot_at;

void identity_via_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd = data[0], op = data[2];
    uint8_t *v = &data[3];
    if (length < 3 + 13) return;
    // Room for name bytes after [cmd][channel][op][status/offset][count].
    uint8_t chunk = length - 5 - (client_wrapper_in_via() ? CLIENT_WRAPPER_OVERHEAD : 0);

    if (cmd == id_custom_get_value && op == IDENTITY_OP_INFO) {
        identity_serial_source_t src;
        const uint8_t           *serial = identity_serial(&src);
        v[0]                            = 1;
        v[1]                            = identity_available();
        v[2]                            = (uint8_t)strlen(identity_name());
        v[3]                            = IDENTITY_NAME_MAX_BYTES;
        v[4]                            = src;
        memcpy(&v[5], serial, IDENTITY_SERIAL_BYTES);
    } else if (cmd == id_custom_get_value && op == IDENTITY_OP_NAME) {
        const char *name = identity_name();
        uint8_t     len = strlen(name), off = v[0];
        uint8_t     n = off < len ? MIN(chunk, len - off) : 0;
        v[0]          = 0;
        v[1]          = n;
        memcpy(&v[2], name + off, n);
    } else if (cmd == id_custom_set_value && op == IDENTITY_OP_NAME) {
        uint8_t off = v[0], n = v[1];
        if (n > chunk || off + n > IDENTITY_NAME_MAX_BYTES) {
            v[0] = IDENTITY_INVALID;
            return;
        }
        memcpy(staged + off, &v[2], n);
        v[0] = IDENTITY_OK;
    } else if (cmd == id_custom_set_value && op == IDENTITY_OP_COMMIT) {
        v[0] = v[0] > IDENTITY_NAME_MAX_BYTES ? IDENTITY_INVALID : identity_set_name(staged, v[0]);
    } else if (cmd == id_custom_set_value && op == IDENTITY_OP_REBOOT) {
        reboot_at = timer_read32() + 100;
        if (!reboot_at) reboot_at = 1;
        v[0] = IDENTITY_OK;
    } else {
        v[0] = IDENTITY_INVALID;
    }
}

void identity_task(void) {
    if (reboot_at && timer_expired32(timer_read32(), reboot_at)) {
        reboot_at = 0;
        soft_reset_keyboard();
    }
}
