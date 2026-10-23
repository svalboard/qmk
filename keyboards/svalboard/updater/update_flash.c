// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Staging-slot erase and program for the updater (see update_flash.h).
//
// Like identity.c and the wear-leveling driver, this goes through the boot ROM
// rather than the Pico SDK's flash wrappers, which refuse addresses above
// PICO_FLASH_SIZE_BYTES (2 MB here). The slot is far above that, so before any
// flash operation the JEDEC capacity must say 16 MiB: on a smaller die these
// offsets would wrap onto the firmware or the settings.

#include <stddef.h>
#include <string.h>
#include "update_flash.h"
#include "identity.h"    // IDENTITY_SECTOR_A
#include "build_reset.h" // BUILD_RESET_SECTOR
#include "update_pair.h" // UPDATE_PAIR_RECORD_OFFSET
#ifdef SVAL_EITHER_HAND
#    include "side.h" // SVAL_SIDE_SECTOR_A, _B
#endif

#ifdef SVAL_UPDATER_HOST_TEST
#    include "update_flash_host.h" // tests/sval_updater: flash and ROM mocks
#else
#    include "pico/bootrom.h"
#    include "hardware/flash.h"
#    include "hardware/sync.h"
#    include "hardware/regs/addressmap.h"
#    define UPDATE_NOCACHE(off) ((const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + (off)))
extern const uint8_t BOOT2_ROM[256];
#endif

// ---- slot layout (D8, R9) -------------------------------------------------------

#define SETTINGS_END (WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES)
_Static_assert(SVAL_UPDATE_MAX_IMAGE % UPDATE_FLASH_BLOCK == 0, "max image is whole 64 KiB blocks");
_Static_assert(SVAL_UPDATE_MAX_IMAGE <= WEAR_LEVELING_RP2040_FLASH_BASE, "an image never reaches the settings");
_Static_assert(SVAL_UPDATE_BASE % UPDATE_FLASH_BLOCK == 0, "slot base is 64 KiB aligned");
_Static_assert(SVAL_UPDATE_SIZE % UPDATE_FLASH_BLOCK == 0, "slot size is whole 64 KiB blocks");
_Static_assert(SVAL_UPDATE_SIZE >= SVAL_UPDATE_MAX_IMAGE, "the slot holds the largest image");
_Static_assert(SVAL_UPDATE_BASE >= SETTINGS_END, "slot starts after both settings copies");
_Static_assert(SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE <= BUILD_RESET_SECTOR, "slot ends before the build reset sector (D39)");
_Static_assert(BUILD_RESET_SECTOR + 0x1000 <= IDENTITY_SECTOR_A, "the build reset sector is below the identity sectors");
_Static_assert(IDENTITY_SECTOR_A < FLASH_LEN, "identity is on the die");
// The pair record sector (docs/pair-uf2.md): a sector of its own, sharing no
// 4 KiB erase sector with the firmware area, the settings, the staging slot,
// the side records, the build reset record or identity, and inside the
// first 4 MiB of the die.
#define PAIR_SECTOR_END (UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_SECTOR_BYTES)
_Static_assert(UPDATE_PAIR_RECORD_OFFSET % UPDATE_PAIR_SECTOR_BYTES == 0, "the pair record is one aligned sector");
_Static_assert(UPDATE_PAIR_SECTOR_BYTES == 4096u && UPDATE_PAIR_PAGE_BYTES == UPDATE_FLASH_PAGE, "pair sector geometry");
_Static_assert(UPDATE_PAIR_RECORD_OFFSET >= SVAL_UPDATE_MAX_IMAGE && UPDATE_PAIR_RECORD_OFFSET >= SETTINGS_END, "the pair record is above the firmware area and the settings");
_Static_assert(PAIR_SECTOR_END <= SVAL_UPDATE_BASE, "the pair record is below the staging slot");
_Static_assert(PAIR_SECTOR_END <= 0x400000u, "the pair record is inside the first 4 MiB");
_Static_assert(PAIR_SECTOR_END <= BUILD_RESET_SECTOR && PAIR_SECTOR_END <= IDENTITY_SECTOR_A, "the pair record is below the build reset and identity sectors");
#ifdef SVAL_EITHER_HAND
_Static_assert(PAIR_SECTOR_END <= SVAL_SIDE_SECTOR_A && PAIR_SECTOR_END <= SVAL_SIDE_SECTOR_B, "the pair record is below the side records");
#endif

#define SLOT_END (SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE)
#define JEDEC_CAPACITY_16MB 0x18 // 2^24 bytes
#define ERASE_BLOCK_CMD 0xD8     // 64 KiB block erase, as the SDK passes to the ROM

static bool     probed;
static bool     available;
static uint8_t  jedec[UPDATE_JEDEC_BYTES];
static uint32_t boot2_copy[64];
static uint8_t  page_buf[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));

// ---- flash access (runs from RAM: XIP is off while the die is busy) -------------

// The absolute range check is repeated here, next to the ROM calls, so that no
// caller mistake can reach flash outside the slot.
static bool __no_inline_not_in_flash_func(abs_in_slot)(uint32_t abs, uint32_t len) {
    return abs >= SVAL_UPDATE_BASE && abs <= SLOT_END && len <= SLOT_END - abs;
}

// page: program that 256 B page at abs. Otherwise erase erase_len bytes at abs,
// which must be one 4 KiB sector or one 64 KiB block.
static void __no_inline_not_in_flash_func(slot_flash_op)(uint32_t abs, const uint8_t *page, uint32_t erase_len) {
    uint32_t len = page ? UPDATE_FLASH_PAGE : erase_len;
    if (!page && erase_len != UPDATE_FLASH_SECTOR && erase_len != UPDATE_FLASH_BLOCK) return;
    if (abs % len || !abs_in_slot(abs, len)) return;
    rom_connect_internal_flash_fn connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn    program  = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn      flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);

    __compiler_memory_barrier();
    connect();
    exit_xip();
    if (page) {
        program(abs, page, UPDATE_FLASH_PAGE);
    } else {
        // As identity.c does: the ROM uses a 64 KiB block erase where the range
        // covers an aligned block, and 4 KiB sector erases otherwise.
        erase(abs, erase_len, UPDATE_FLASH_BLOCK, ERASE_BLOCK_CMD);
    }
    flush();
#ifdef SVAL_UPDATER_HOST_TEST
    update_host_enter_xip(boot2_copy);
#else
    ((void (*)(void))((intptr_t)boot2_copy + 1))(); // back to XIP via boot2
#endif
}

// The pair record sector: erase it, or program its state page. Fixed
// addresses, so no argument can move either onto anything else.
static void __no_inline_not_in_flash_func(pair_flash_op)(const uint8_t *state_page) {
    rom_connect_internal_flash_fn connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn    program  = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn      flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);

    __compiler_memory_barrier();
    connect();
    exit_xip();
    if (state_page) {
        program(UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_STATE_OFFSET, state_page, UPDATE_FLASH_PAGE);
    } else {
        erase(UPDATE_PAIR_RECORD_OFFSET, UPDATE_PAIR_SECTOR_BYTES, UPDATE_FLASH_BLOCK, ERASE_BLOCK_CMD);
    }
    flush();
#ifdef SVAL_UPDATER_HOST_TEST
    update_host_enter_xip(boot2_copy);
#else
    ((void (*)(void))((intptr_t)boot2_copy + 1))(); // back to XIP via boot2
#endif
}

static uint8_t read_jedec(uint8_t id[UPDATE_JEDEC_BYTES]) {
    uint8_t  tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, sizeof(tx));
    restore_interrupts(irq);
    memcpy(id, &rx[1], UPDATE_JEDEC_BYTES);
    return rx[3];
}

// ---- public API -------------------------------------------------------------------

void update_flash_init(void) {
    if (probed) return;
    probed = true;
    // Taken at init, while page 0 is still this firmware's boot2.
    memcpy(boot2_copy, BOOT2_ROM, sizeof(boot2_copy));
    available = read_jedec(jedec) == JEDEC_CAPACITY_16MB;
}

bool update_flash_available(void) {
    update_flash_init();
    return available;
}

void update_flash_jedec(uint8_t id[UPDATE_JEDEC_BYTES]) {
    update_flash_init();
    memcpy(id, jedec, UPDATE_JEDEC_BYTES);
}

bool update_slot_range_ok(uint32_t off, uint32_t len) {
    return off <= SVAL_UPDATE_SIZE && len <= SVAL_UPDATE_SIZE - off;
}

const volatile uint8_t *update_slot_read(uint32_t off, uint32_t len) {
    if (!update_slot_range_ok(off, len)) return NULL;
    return UPDATE_NOCACHE(SVAL_UPDATE_BASE + off);
}

// Whole words: each uncached read is a flash transaction. off and len are
// page-aligned wherever this is called.
static bool range_is(uint32_t off, uint32_t len, uint8_t value) {
    const volatile uint32_t *p    = (const volatile uint32_t *)UPDATE_NOCACHE(SVAL_UPDATE_BASE + off);
    uint32_t                 word = value * 0x01010101u;
    for (uint32_t i = 0; i < len / 4; i++) {
        if (p[i] != word) return false;
    }
    return true;
}

static bool unit_ok(uint32_t off, uint32_t size) {
    return off % size == 0 && update_slot_range_ok(off, size);
}

static update_status_t erase_unit(uint32_t off, uint32_t size) {
    if (!unit_ok(off, size)) return UPDATE_INVALID;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    if (range_is(off, size, 0xFF)) return UPDATE_OK;
    uint32_t irq = save_and_disable_interrupts();
    slot_flash_op(SVAL_UPDATE_BASE + off, NULL, size);
    restore_interrupts(irq);
    return range_is(off, size, 0xFF) ? UPDATE_OK : UPDATE_FLASH_ERR;
}

bool update_flash_block_erased(uint32_t off) {
    return unit_ok(off, UPDATE_FLASH_BLOCK) && range_is(off, UPDATE_FLASH_BLOCK, 0xFF);
}

bool update_flash_sector_erased(uint32_t off) {
    return unit_ok(off, UPDATE_FLASH_SECTOR) && range_is(off, UPDATE_FLASH_SECTOR, 0xFF);
}

update_status_t update_flash_erase_block(uint32_t off) {
    return erase_unit(off, UPDATE_FLASH_BLOCK);
}

update_status_t update_flash_erase_sector(uint32_t off) {
    return erase_unit(off, UPDATE_FLASH_SECTOR);
}

update_status_t update_flash_program_page(uint32_t off, const uint8_t data[UPDATE_FLASH_PAGE]) {
    if (!data || off % UPDATE_FLASH_PAGE || !update_slot_range_ok(off, UPDATE_FLASH_PAGE)) return UPDATE_INVALID;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    if (!range_is(off, UPDATE_FLASH_PAGE, 0xFF)) return UPDATE_INVALID;
    memcpy(page_buf, data, UPDATE_FLASH_PAGE); // the ROM reads it with XIP off
    uint32_t irq = save_and_disable_interrupts();
    slot_flash_op(SVAL_UPDATE_BASE + off, page_buf, 0);
    restore_interrupts(irq);
    const volatile uint8_t *p = UPDATE_NOCACHE(SVAL_UPDATE_BASE + off);
    for (uint32_t i = 0; i < UPDATE_FLASH_PAGE; i++) {
        if (p[i] != page_buf[i]) return UPDATE_FLASH_ERR;
    }
    return UPDATE_OK;
}

// ---- the pair record sector (update_pair.h) ---------------------------------------

const volatile uint8_t *update_pair_sector_read(void) {
    return UPDATE_NOCACHE(UPDATE_PAIR_RECORD_OFFSET);
}

update_status_t update_pair_sector_erase(void) {
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    const volatile uint32_t *p = (const volatile uint32_t *)UPDATE_NOCACHE(UPDATE_PAIR_RECORD_OFFSET);
    bool                     erased = true;
    for (uint32_t i = 0; i < UPDATE_PAIR_SECTOR_BYTES / 4 && erased; i++) erased = p[i] == 0xFFFFFFFFu;
    if (erased) return UPDATE_OK;
    uint32_t irq = save_and_disable_interrupts();
    pair_flash_op(NULL);
    restore_interrupts(irq);
    for (uint32_t i = 0; i < UPDATE_PAIR_SECTOR_BYTES / 4; i++) {
        if (p[i] != 0xFFFFFFFFu) return UPDATE_FLASH_ERR;
    }
    return UPDATE_OK;
}

update_status_t update_pair_state_program(const uint8_t data[UPDATE_FLASH_PAGE]) {
    if (!data) return UPDATE_INVALID;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    const volatile uint8_t *p = UPDATE_NOCACHE(UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_STATE_OFFSET);
    memcpy(page_buf, data, UPDATE_FLASH_PAGE); // the ROM reads it with XIP off
    uint8_t want[UPDATE_FLASH_PAGE];
    for (uint32_t i = 0; i < UPDATE_FLASH_PAGE; i++) want[i] = p[i] & page_buf[i];
    uint32_t irq = save_and_disable_interrupts();
    pair_flash_op(page_buf);
    restore_interrupts(irq);
    for (uint32_t i = 0; i < UPDATE_FLASH_PAGE; i++) {
        if (p[i] != want[i]) return UPDATE_FLASH_ERR;
    }
    return UPDATE_OK;
}
