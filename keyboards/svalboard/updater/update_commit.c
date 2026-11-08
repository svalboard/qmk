// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The commit (plan M1, "Commit routine", steps 0-7; docs/updater.md).
//
// Step 0 runs from flash with interrupts on: it re-checks everything and gets
// ready. Steps 1-7 run from RAM (the commit_ram_* functions, each in its own
// .time_critical section) with PRIMASK set, while the firmware area they
// rewrite is erased. They call nothing but the boot ROM, through pointers
// looked up in step 0, the commit's own boot2 copy, and each other.
// kb/tools/check_ram_funcs.py checks that on the linked ELF. Rules for the RAM
// code, so that nothing in it can reach flash while XIP is off:
//   - no switch (jump tables, libgcc case helpers), no division, no struct
//     copies, no memcpy/memset; loops are built with
//     -fno-tree-loop-distribute-patterns so GCC cannot turn them into calls;
//   - helpers are always_inline; every register access is a plain volatile
//     access (the HAL below);
//   - no const data: GCC would place it in flash .rodata.
//
// Host tests. The same step logic compiles on the host (SVAL_UPDATER_HOST_TEST)
// against tests/sval_updater/update_commit_host.h, which supplies the HAL: the
// register file, PRIMASK, flash reads, the XIP re-entry, the reset trigger
// and the test-hook halts are mocks there, and the ROM pointers come from the
// same mock die as the staging tests. Only the HAL section differs.

#include <stddef.h>
#include <string.h>
#include "update_commit.h"
#include "update_flash.h"
#include "update_image.h"
#include "update_led.h"
#include "updater_port.h"
#include "sha512.h" // vendor/: SHA-512 from Monocypher

// ---- registers (RP2040 datasheet; checked against the SDK headers below) -----------

#define WD_BASE 0x40058000u
#define WD_CTRL (WD_BASE + 0x00u)
#define WD_LOAD (WD_BASE + 0x04u)
#define WD_SCRATCH4 (WD_BASE + 0x1Cu)
#define WD_TICK (WD_BASE + 0x2Cu)
#define WD_CTRL_TRIGGER 0x80000000u
#define WD_CTRL_ENABLE 0x40000000u
#define WD_CTRL_PAUSE 0x07000000u // PAUSE_DBG1, PAUSE_DBG0, PAUSE_JTAG: set at reset
#define WD_TICK_RUNNING 0x00000400u
#define PSM_WDSEL 0x40010008u
#define PSM_WDSEL_ALL_BUT_OSC 0x0001FFFCu // everything but ROSC and XOSC, as _watchdog_enable does
#define DMA_CTRL_TRIG(ch) (0x50000000u + 0x40u * (ch) + 0x0Cu)
#define DMA_CTRL_BUSY 0x01000000u
#define DMA_CHANNELS 12
#define PPB_VTOR 0xE000ED08u
#define REG_ALIAS_SET 0x2000u // RP2040 atomic set / clear aliases
#define REG_ALIAS_CLR 0x3000u

#define VECTOR_SLOTS 48 // 16 system + 32 IRQs
#define WD_LOAD_VALUE (UPDATE_COMMIT_WATCHDOG_US * 2u)
_Static_assert(WD_LOAD_VALUE <= 0xFFFFFFu, "watchdog LOAD is 24 bits");
_Static_assert(UPDATE_COMMIT_WATCHDOG_US >= 2u * 2000000u, "at least twice the worst 64 KiB block erase");

#define PAGE UPDATE_FLASH_PAGE
#define SECTOR UPDATE_FLASH_SECTOR
#define BLOCK UPDATE_FLASH_BLOCK
#define ERASE_BLOCK_CMD 0xD8 // as the SDK passes to the ROM
#define MAX_IMAGE SVAL_UPDATE_MAX_IMAGE
_Static_assert(MAX_IMAGE % BLOCK == 0, "the commit erases whole blocks up to MAX_IMAGE");
_Static_assert(MAX_IMAGE <= SVAL_UPDATE_BASE, "the commit's range never reaches the slot");

// The RP2040 double-tap magic (platforms/chibios/bootloaders/rp2040.c). Built
// at run time from its complement, so the word itself never appears in the
// RAM code that sweeps RAM for it (check_ram_funcs.py checks that too).
#define DOUBLE_TAP_MAGIC_NOT 0x35014F45u // ~0xCAFEB0BA

// ---- HAL --------------------------------------------------------------------------------

#define COMMIT_INLINE static inline __attribute__((always_inline))

#ifdef SVAL_UPDATER_HOST_TEST
#    include "update_commit_host.h" // tests/sval_updater: mock registers, die and halts
#    define COMMIT_RAM(name) name
typedef uintptr_t commit_vec_t;
COMMIT_INLINE uint32_t reg_rd(uint32_t addr) {
    return update_host_reg_rd(addr);
}
COMMIT_INLINE void reg_wr(uint32_t addr, uint32_t v) {
    update_host_reg_wr(addr, v);
}
COMMIT_INLINE uint32_t flash_rd32(uint32_t off) {
    return update_host_flash_rd32(off);
}
COMMIT_INLINE void irq_disable(void) {
    update_host_primask(true);
}
COMMIT_INLINE void irq_enable(void) {
    update_host_primask(false);
}
COMMIT_INLINE void barrier(void) {}
COMMIT_INLINE void enter_xip(const uint32_t *boot2) {
    update_host_enter_xip(boot2);
}
COMMIT_INLINE volatile uint32_t *ram0_word(uint32_t i) {
    return &update_host_ram0[i];
}
#    define RAM0_WORDS UPDATE_HOST_RAM0_WORDS
COMMIT_INLINE uint32_t vec_addr(const volatile commit_vec_t *v) {
    return update_host_vtor_value(v);
}
COMMIT_INLINE commit_vec_t vec_handler(void (*f)(void)) {
    return (uintptr_t)f;
}
COMMIT_INLINE uint32_t initial_sp(void) {
    return UPDATE_HOST_INITIAL_SP;
}
COMMIT_INLINE bool rom_ptr_ok(void *p) {
    return update_host_rom_ptr_ok(p);
}
COMMIT_INLINE uint32_t double_tap_magic(void) {
    return ~DOUBLE_TAP_MAGIC_NOT;
}
COMMIT_INLINE void __attribute__((noreturn)) halt_spin(bool fed) {
    update_host_halt(fed);
}
COMMIT_INLINE void fault_now(void) {
    update_host_fault();
}
COMMIT_INLINE void __attribute__((noreturn)) spin_forever(void) {
    update_host_spin();
}
#else
#    include "pico/bootrom.h"
#    include "hardware/regs/addressmap.h"
#    include "hardware/regs/watchdog.h"
#    include "hardware/regs/psm.h"
#    include "hardware/regs/dma.h"
#    include "hardware/regs/m0plus.h"
#    include "hardware/platform_defs.h"
// noclone: GCC 15 otherwise splits an unexported RAM function with IPA-SRA
// into "name.isra.0", and check_ram_funcs.py no longer finds "name" (the
// clone is still in RAM; GCC 13.2 emits identical code either way).
#    define COMMIT_RAM(name) __attribute__((noclone)) __no_inline_not_in_flash_func(name)
typedef uint32_t commit_vec_t;
_Static_assert(WD_CTRL == WATCHDOG_BASE + WATCHDOG_CTRL_OFFSET && WD_LOAD == WATCHDOG_BASE + WATCHDOG_LOAD_OFFSET, "watchdog registers");
_Static_assert(WD_SCRATCH4 == WATCHDOG_BASE + WATCHDOG_SCRATCH4_OFFSET && WD_TICK == WATCHDOG_BASE + WATCHDOG_TICK_OFFSET, "watchdog registers");
_Static_assert(WD_CTRL_TRIGGER == WATCHDOG_CTRL_TRIGGER_BITS && WD_CTRL_ENABLE == WATCHDOG_CTRL_ENABLE_BITS, "watchdog bits");
_Static_assert(WD_CTRL_PAUSE == (WATCHDOG_CTRL_PAUSE_DBG0_BITS | WATCHDOG_CTRL_PAUSE_DBG1_BITS | WATCHDOG_CTRL_PAUSE_JTAG_BITS), "watchdog bits");
_Static_assert(WD_TICK_RUNNING == WATCHDOG_TICK_RUNNING_BITS, "watchdog tick bit");
_Static_assert(PSM_WDSEL == PSM_BASE + PSM_WDSEL_OFFSET, "PSM_WDSEL");
_Static_assert(PSM_WDSEL_ALL_BUT_OSC == (PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS)), "PSM_WDSEL value");
_Static_assert(DMA_CTRL_TRIG(1) == DMA_BASE + DMA_CH1_CTRL_TRIG_OFFSET && DMA_CTRL_BUSY == DMA_CH0_CTRL_TRIG_BUSY_BITS, "DMA");
_Static_assert(DMA_CHANNELS == NUM_DMA_CHANNELS, "DMA channels");
_Static_assert(PPB_VTOR == PPB_BASE + M0PLUS_VTOR_OFFSET, "VTOR");
_Static_assert(VECTOR_SLOTS == 16 + 32, "RP2040 has 32 IRQs");
COMMIT_INLINE uint32_t reg_rd(uint32_t addr) {
    return *(volatile uint32_t *)addr;
}
COMMIT_INLINE void reg_wr(uint32_t addr, uint32_t v) {
    *(volatile uint32_t *)addr = v;
}
COMMIT_INLINE uint32_t flash_rd32(uint32_t off) { // uncached: always the die, never stale lines
    return *(volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + off);
}
COMMIT_INLINE void irq_disable(void) {
    __asm volatile("cpsid i" ::: "memory");
}
COMMIT_INLINE void irq_enable(void) {
    __asm volatile("cpsie i" ::: "memory");
}
COMMIT_INLINE void barrier(void) {
    __asm volatile("dsb\n\tisb" ::: "memory");
}
COMMIT_INLINE void enter_xip(const uint32_t *boot2) {
    ((void (*)(void))((uintptr_t)boot2 + 1))(); // boot2 sets XIP up again and returns
}
COMMIT_INLINE volatile uint32_t *ram0_word(uint32_t i) {
    return (volatile uint32_t *)(SRAM_BASE + 4u * i);
}
#    define RAM0_WORDS ((SRAM4_BASE - SRAM_BASE) / 4u) // striped SRAM0-3; the stacks are in SRAM4
COMMIT_INLINE uint32_t vec_addr(const volatile commit_vec_t *v) {
    return (uint32_t)v;
}
COMMIT_INLINE commit_vec_t vec_handler(void (*f)(void)) {
    return (uint32_t)f; // a Thumb function address: bit 0 already set
}
COMMIT_INLINE uint32_t initial_sp(void) {
    return *(const volatile uint32_t *)reg_rd(PPB_VTOR); // slot 0 of the running vector table
}
COMMIT_INLINE bool rom_ptr_ok(void *p) {
    uintptr_t a = (uintptr_t)p;
    return (a & 1u) && a < 0x4000u; // a Thumb address inside the boot ROM
}
COMMIT_INLINE uint32_t double_tap_magic(void) {
    uint32_t m = DOUBLE_TAP_MAGIC_NOT;
    __asm volatile("" : "+r"(m)); // keep GCC from folding the complement back into a literal
    return ~m;
}
COMMIT_INLINE void __attribute__((noreturn)) spin_forever(void) {
    for (;;) {
    }
}
COMMIT_INLINE void fault_now(void) {
    __asm volatile("udf #0" ::: "memory"); // HardFault, through the RAM vector table
}
COMMIT_INLINE void __attribute__((noreturn)) halt_spin(bool fed) {
    for (;;) {
        if (fed) reg_wr(WD_LOAD, WD_LOAD_VALUE);
    }
}
#endif

// ---- the commit's own RAM ---------------------------------------------------------------

// The boot ROM's flash functions, looked up in step 0 (before PRIMASK).
typedef struct {
    rom_connect_internal_flash_fn connect;
    rom_flash_exit_xip_fn         exit_xip;
    rom_flash_range_erase_fn      erase;
    rom_flash_range_program_fn    program;
    rom_flash_flush_cache_fn      flush;
} commit_rom_t;

static commit_rom_t commit_rom;
static uint32_t     commit_boot2[UPDATE_BOOT2_BYTES / 4];             // step 0's copy of the running boot2, never refilled
static uint32_t     commit_sector_buf[SECTOR / 4];                    // dedicated; not shared with QMK
static commit_vec_t commit_vectors[VECTOR_SLOTS] __attribute__((aligned(256))); // VTOR: aligned to the table size, rounded up
static uint32_t     image_len, image_crc, page0_crc, erase_end, vector_sp;
#ifdef SVAL_UPDATE_TEST_HOOKS
static uint8_t halt_point;
static bool    halt_fed;
#endif

#define BUF8 ((uint8_t *)commit_sector_buf)
#define SLOT_ABS(off) (SVAL_UPDATE_BASE + (off))

// ---- RAM helpers (always inlined into the RAM functions) --------------------------------

COMMIT_INLINE void feed(void) {
    reg_wr(WD_LOAD, WD_LOAD_VALUE);
}

COMMIT_INLINE uint32_t crc_word(uint32_t crc, uint32_t w) {
    for (uint32_t i = 0; i < 4; i++) {
        crc ^= ((w >> (8 * i)) & 0xFFu) << 24; // the bytes in address order
        for (uint32_t b = 0; b < 8; b++) crc = (crc << 1) ^ (0x04C11DB7u & -(crc >> 31));
    }
    return crc;
}

// CRC-32/MPEG-2 of firmware bytes [0x100, image_len), read back uncached.
COMMIT_INLINE uint32_t firmware_crc(void) {
    uint32_t crc = UPDATE_CRC32_INIT;
    for (uint32_t off = UPDATE_VECTORS_OFFSET; off < image_len; off += 4) {
        crc = crc_word(crc, flash_rd32(off));
        if ((off & (SECTOR - 1)) == 0) feed();
    }
    return crc;
}

// Zeroes the sector buffer and every SRAM0-3 word equal to the double-tap
// magic: the new image's noinit magic word could land on old image bytes, and
// the reset would then send it to BOOTSEL (R23).
COMMIT_INLINE void wipe_ram(void) {
    for (uint32_t i = 0; i < SECTOR / 4; i++) commit_sector_buf[i] = 0;
    uint32_t magic = double_tap_magic();
    for (uint32_t i = 0; i < RAM0_WORDS; i++) {
        volatile uint32_t *w = ram0_word(i);
        if (*w == magic) *w = 0;
    }
}

// ---- RAM functions ----------------------------------------------------------------------

// Step 7, the only way out once step 1 has begun, and the handler in every slot
// of the RAM vector table: wipe_ram(), then the watchdog reset, with PSM_WDSEL
// confirmed and scratch4 cleared so the boot ROM boots from flash. Never calls
// flash.
static void __attribute__((noreturn)) COMMIT_RAM(commit_ram_reset)(void) {
    irq_disable();
    wipe_ram();
    if (reg_rd(PSM_WDSEL) != PSM_WDSEL_ALL_BUT_OSC) reg_wr(PSM_WDSEL, PSM_WDSEL_ALL_BUT_OSC);
    reg_wr(WD_SCRATCH4, 0);
    reg_wr(WD_CTRL + REG_ALIAS_SET, WD_CTRL_TRIGGER);
    spin_forever();
}

// One boot ROM erase or program inside the firmware area [0, MAX_IMAGE), and
// nothing else: src NULL erases len bytes at off, which must be one 4 KiB
// sector or one 64 KiB block, aligned to its size; otherwise len bytes from
// src (a multiple of 256, at most one sector, page-aligned) are programmed at
// off. Anything else returns false with no ROM call. XIP comes back through
// step 0's boot2 copy, and the watchdog is fed after every ROM call.
static bool COMMIT_RAM(commit_ram_flash)(uint32_t off, const uint8_t *src, uint32_t len) {
    if (off >= MAX_IMAGE || len == 0 || len > MAX_IMAGE - off) return false;
    if (src) {
        if (len > SECTOR || (len & (PAGE - 1)) || (off & (PAGE - 1))) return false;
    } else if (!(len == SECTOR && (off & (SECTOR - 1)) == 0) && !(len == BLOCK && (off & (BLOCK - 1)) == 0)) {
        return false;
    }
    __asm volatile("" ::: "memory");
    commit_rom.connect();
    commit_rom.exit_xip();
    if (src) {
        commit_rom.program(off, src, len);
    } else {
        commit_rom.erase(off, len, BLOCK, ERASE_BLOCK_CMD);
    }
    commit_rom.flush();
    enter_xip(commit_boot2);
    feed();
    return true;
}

// Step 4 for one sector: slot -> sector buffer with XIP on, then the ROM
// program. Sector 0 starts at page 1: page 0 is written last (step 6).
static bool COMMIT_RAM(commit_ram_sector)(uint32_t off) {
    uint32_t from = off == 0 ? PAGE : 0;
    uint32_t n    = image_len - off < SECTOR ? image_len - off : SECTOR;
    for (uint32_t i = from / 4; i < n / 4; i++) commit_sector_buf[i] = flash_rd32(SLOT_ABS(off) + 4 * i);
    return commit_ram_flash(off + from, BUF8 + from, n - from);
}

#ifdef SVAL_UPDATE_TEST_HOOKS
// A halt leaves RAM as step 7 would, so an unfed halt's watchdog reset starts
// the next image from the same RAM state as a real commit's reset.
static void __attribute__((noreturn)) COMMIT_RAM(commit_ram_halt)(void) {
    if (halt_point == UPDATE_HALT_FAULT || halt_point == UPDATE_HALT_FAULT_ERASED) fault_now();
    wipe_ram();
    halt_spin(halt_fed);
}
#    define HALT(point)                                \
        do {                                           \
            if (halt_point == (point)) commit_ram_halt(); \
        } while (0)
#else
#    define HALT(point) \
        do {            \
        } while (0)
#endif

// Steps 1-7. Returns only to refuse at step 1 (a DMA channel busy), before
// anything is changed; after that every path ends in commit_ram_reset().
static update_status_t COMMIT_RAM(commit_ram_main)(void) {
    // 1. PRIMASK; no DMA may be running (the PMW SPI TX source is in XIP, R6).
    irq_disable();
    for (uint32_t ch = 0; ch < DMA_CHANNELS; ch++) {
        if (reg_rd(DMA_CTRL_TRIG(ch)) & DMA_CTRL_BUSY) {
            irq_enable();
            return UPDATE_BUSY;
        }
    }
    // The RAM vector table: any fault from here on resets (R3).
    commit_vectors[0] = vector_sp;
    for (uint32_t i = 1; i < VECTOR_SLOTS; i++) commit_vectors[i] = vec_handler(commit_ram_reset);
    __asm volatile("" ::: "memory"); // the table is written before VTOR points at it
    reg_wr(PPB_VTOR, vec_addr(commit_vectors));
    barrier();
    // The watchdog (D9), in this order: PSM_WDSEL first (it resets to 0, and a
    // watchdog reset with 0 resets nothing), scratch4, LOAD, the pause bits off
    // (they are set at reset), then ENABLE.
    reg_wr(PSM_WDSEL, PSM_WDSEL_ALL_BUT_OSC);
    reg_wr(WD_SCRATCH4, 0);
    reg_wr(WD_LOAD, WD_LOAD_VALUE);
    reg_wr(WD_CTRL + REG_ALIAS_CLR, WD_CTRL_PAUSE);
    reg_wr(WD_CTRL + REG_ALIAS_SET, WD_CTRL_ENABLE);

    // 2. Invalidate: page 0 to all zeros, read back (R1). A zero page 0 fails
    // the boot ROM's boot2 CRC, so from here on a cut ends in BOOTSEL.
    for (uint32_t i = 0; i < PAGE / 4; i++) commit_sector_buf[i] = 0;
    if (!commit_ram_flash(0, BUF8, PAGE)) commit_ram_reset();
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        if (flash_rd32(4 * i) != 0) commit_ram_reset(); // the old image is still mostly intact
    }
    HALT(UPDATE_HALT_INVALIDATED);
    HALT(UPDATE_HALT_FAULT);

    // 3. Erase sector 0, the rest of block 0 a sector at a time, then whole
    // blocks up to round_up(image_len, 64 KiB).
    if (!commit_ram_flash(0, NULL, SECTOR)) commit_ram_reset();
    HALT(UPDATE_HALT_FIRST_ERASE);
    HALT(UPDATE_HALT_FAULT_ERASED);
    for (uint32_t off = SECTOR; off < BLOCK; off += SECTOR) {
        if (!commit_ram_flash(off, NULL, SECTOR)) commit_ram_reset();
    }
    for (uint32_t off = BLOCK; off < erase_end; off += BLOCK) {
        if (!commit_ram_flash(off, NULL, BLOCK)) commit_ram_reset();
    }

    // 4. Copy and program, a sector at a time.
    uint32_t sectors = (image_len + SECTOR - 1) / SECTOR; // a shift: SECTOR is a power of two
    for (uint32_t s = 0; s < sectors; s++) {
        if (!commit_ram_sector(s * SECTOR)) commit_ram_reset();
        if (s == sectors / 2) HALT(UPDATE_HALT_MID_PROGRAM);
    }
    HALT(UPDATE_HALT_LAST_SECTOR);

    // 5. Read it all back. On a mismatch, every sector that differs from the
    // slot is erased and programmed again, once; if the CRC still fails, page
    // 0 stays erased and the reset lands in BOOTSEL.
    if (firmware_crc() != image_crc) {
        for (uint32_t off = 0; off < image_len; off += SECTOR) {
            uint32_t end = image_len - off < SECTOR ? image_len : off + SECTOR;
            bool     bad = false;
            for (uint32_t a = off == 0 ? PAGE : off; a < end; a += 4) bad |= flash_rd32(a) != flash_rd32(SLOT_ABS(a));
            if (!bad) continue;
            if (!commit_ram_flash(off, NULL, SECTOR) || !commit_ram_sector(off)) commit_ram_reset();
        }
        if (firmware_crc() != image_crc) commit_ram_reset();
    }

    // 6. Page 0, last: the verified bytes (step 0's CRC of them), then read back.
    uint32_t crc = UPDATE_CRC32_INIT;
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        commit_sector_buf[i] = flash_rd32(SLOT_ABS(4 * i));
        crc           = crc_word(crc, commit_sector_buf[i]);
    }
    if (crc != page0_crc) commit_ram_reset(); // page 0 is still erased: BOOTSEL
    if (!commit_ram_flash(0, BUF8, PAGE)) commit_ram_reset();
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        if (flash_rd32(4 * i) != commit_sector_buf[i]) {
            // Not what was meant: zero it (NOR can always clear bits), so the
            // boot ROM goes to BOOTSEL rather than run a damaged boot2.
            for (uint32_t k = 0; k < PAGE / 4; k++) commit_sector_buf[k] = 0;
            commit_ram_flash(0, BUF8, PAGE);
            commit_ram_reset();
        }
    }
    HALT(UPDATE_HALT_PAGE0);

    // 7. Into the new image.
    commit_ram_reset();
}

// ---- step 0 (flash, interrupts on) --------------------------------------------------------

bool update_commit_available(void) {
    return update_flash_available();
}

#ifdef SVAL_UPDATE_TEST_HOOKS
bool update_commit_test_halt(uint8_t point, bool fed) {
    if (point >= UPDATE_HALT_COUNT) return false;
    halt_point = point;
    halt_fed   = fed;
    return true;
}
#endif

// SHA-512 of the slot against the manifest, the body CRC against the one
// verified, and the head checks; also page 0's CRC for step 6. Reads through
// the sector buffer.
static update_status_t check_slot(const sval_update_manifest_t *m, uint32_t crc_body) {
    crypto_sha512_ctx sha;
    uint32_t          crc = UPDATE_CRC32_INIT;
    update_status_t   st  = UPDATE_OK;
    crypto_sha512_init(&sha);
    for (uint32_t off = 0; off < m->image_len; off += SECTOR) {
        uint32_t n = m->image_len - off < SECTOR ? m->image_len - off : SECTOR;
        for (uint32_t i = 0; i < n / 4; i++) commit_sector_buf[i] = flash_rd32(SLOT_ABS(off) + 4 * i);
        crypto_sha512_update(&sha, BUF8, n);
        if (off == 0) {
            st        = update_image_head_check(BUF8, m->image_len);
            page0_crc = update_crc32_mpeg2(BUF8, PAGE);
            crc       = update_crc32_mpeg2_update(crc, BUF8 + PAGE, n - PAGE);
        } else {
            crc = update_crc32_mpeg2_update(crc, BUF8, n);
        }
    }
    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512_final(&sha, hash);
    if (memcmp(hash, m->sha512, sizeof(hash)) != 0 || crc != crc_body) return UPDATE_BAD_HASH;
    return st;
}

static void wipe_buffer(void) {
    for (uint32_t i = 0; i < SECTOR / 4; i++) commit_sector_buf[i] = 0;
}

static update_status_t prepare(const sval_update_manifest_t *m, uint32_t crc_body) {
    // D12: settings writes must not be failing. The identity task is already
    // held off: svalboard.c skips it while updater_active().
    if (updater_port_settings_failing() || !update_flash_available()) return UPDATE_UNAVAILABLE;
    update_device_t dev;
    update_device_self(&dev);
    update_status_t st = update_manifest_check(m, &dev);
    if (st != UPDATE_OK) return st;
    st = check_slot(m, crc_body);
    if (st != UPDATE_OK) return st;

    // The commit's own boot2 copy, from page 0 while it is still this
    // firmware's (BOOT2_ROM is page 0; after step 2 it reads zeros).
    for (uint32_t i = 0; i < UPDATE_BOOT2_BYTES / 4; i++) commit_boot2[i] = flash_rd32(4 * i);
    if (!update_boot2_valid((const uint8_t *)commit_boot2)) return UPDATE_FLASH_ERR;

    commit_rom.connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    commit_rom.exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    commit_rom.erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    commit_rom.program  = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    commit_rom.flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    if (!rom_ptr_ok((void *)commit_rom.connect) || !rom_ptr_ok((void *)commit_rom.exit_xip) || !rom_ptr_ok((void *)commit_rom.erase) || !rom_ptr_ok((void *)commit_rom.program) || !rom_ptr_ok((void *)commit_rom.flush)) return UPDATE_UNAVAILABLE;
    // The watchdog counts watchdog ticks: without them it would never fire.
    if (!(reg_rd(WD_TICK) & WD_TICK_RUNNING)) return UPDATE_UNAVAILABLE;

    image_len = m->image_len;
    image_crc = crc_body;
    erase_end = (image_len + BLOCK - 1) / BLOCK * BLOCK;
    if (erase_end > MAX_IMAGE) erase_end = MAX_IMAGE;
    vector_sp = initial_sp();
    return UPDATE_OK;
}

update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body) {
    update_status_t st = prepare(m, crc_body);
    if (st == UPDATE_OK) {
        // The writing color, latched; the last WS2812 transfer finishes
        // before step 1 checks the DMA channels.
        update_led_commit_latch();
        st = commit_ram_main(); // returns only to refuse, with nothing erased
    }
    wipe_buffer();
    return st;
}
