/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2022 Nick Brassel (@tzarc)
 * Copyright (c) 2022 Stefan Kerkmann (@KarlK90)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>

#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/ssi.h"
#include "hardware/structs/ioqspi.h"

#include "compiler_support.h"
#include "timer.h"
#include "wear_leveling.h"
#include "wear_leveling_rp2040_flash_config.h"
#include "wear_leveling_internal.h"

#ifndef WEAR_LEVELING_RP2040_FLASH_BULK_COUNT
#    define WEAR_LEVELING_RP2040_FLASH_BULK_COUNT 64
#endif // WEAR_LEVELING_RP2040_FLASH_BULK_COUNT

#define FLASHCMD_PAGE_PROGRAM 0x02
#define FLASHCMD_READ_STATUS 0x05
#define FLASHCMD_WRITE_ENABLE 0x06

extern const uint8_t BOOT2_ROM[256];
static uint32_t      BOOT2_ROM_RAM[64];

static ssi_hw_t *const ssi = (ssi_hw_t *)XIP_SSI_BASE;

// Sanity check
check_hw_layout(ssi_hw_t, ssienr, SSI_SSIENR_OFFSET);
check_hw_layout(ssi_hw_t, spi_ctrlr0, SSI_SPI_CTRLR0_OFFSET);

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    ((void (*)(void))BOOT2_ROM_RAM + 1)();
}

// Bitbanging the chip select using IO overrides, in case RAM-resident IRQs
// are still running, and the FIFO bottoms out. (the bootrom does the same)
static void __no_inline_not_in_flash_func(flash_cs_force)(bool high) {
    uint32_t field_val = high ? IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH : IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    hw_write_masked(&ioqspi_hw->io[1].ctrl, field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB, IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS);
}

// Also allow any unbounded loops to check whether the above abort condition
// was asserted, and terminate early
static int __no_inline_not_in_flash_func(flash_was_aborted)(void) {
    return *(io_rw_32 *)(IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD1_CTRL_OFFSET) & IO_QSPI_GPIO_QSPI_SD1_CTRL_INOVER_BITS;
}

// Put bytes from one buffer, and get bytes into another buffer.
// These can be the same buffer.
// If tx is NULL then send zeroes.
// If rx is NULL then all read data will be dropped.
//
// If rx_skip is nonzero, this many bytes will first be consumed from the FIFO,
// before reading a further count bytes into *rx.
// E.g. if you have written a command+address just before calling this function.
static void __no_inline_not_in_flash_func(flash_put_get)(const uint8_t *tx, uint8_t *rx, size_t count, size_t rx_skip) {
    // Make sure there is never more data in flight than the depth of the RX
    // FIFO. Otherwise, when we are interrupted for long periods, hardware
    // will overflow the RX FIFO.
    const uint max_in_flight = 16 - 2; // account for data internal to SSI
    size_t     tx_count      = count;
    size_t     rx_count      = count;
    while (tx_count || rx_skip || rx_count) {
        // NB order of reads, for pessimism rather than optimism
        uint32_t tx_level      = ssi_hw->txflr;
        uint32_t rx_level      = ssi_hw->rxflr;
        bool     did_something = false; // Expect this to be folded into control flow, not register
        if (tx_count && tx_level + rx_level < max_in_flight) {
            ssi->dr0 = (uint32_t)(tx ? *tx++ : 0);
            --tx_count;
            did_something = true;
        }
        if (rx_level) {
            uint8_t rxbyte = ssi->dr0;
            did_something  = true;
            if (rx_skip) {
                --rx_skip;
            } else {
                if (rx) *rx++ = rxbyte;
                --rx_count;
            }
        }
        // APB load costs 4 cycles, so only do it on idle loops (our budget is
        // 48 cyc/byte)
        if (!did_something && __builtin_expect(flash_was_aborted(), 0)) break;
    }
    flash_cs_force(1);
}

// Convenience wrapper for above
// (And it's hard for the debug host to get the tight timing between
// cmd DR0 write and the remaining data)
static void __no_inline_not_in_flash_func(_flash_do_cmd)(uint8_t cmd, const uint8_t *tx, uint8_t *rx, size_t count) {
    flash_cs_force(0);
    ssi->dr0 = cmd;
    flash_put_get(tx, rx, count, 1);
}

// Timing of this one is critical, so do not expose the symbol to debugger etc
static void __no_inline_not_in_flash_func(flash_put_cmd_addr)(uint8_t cmd, uint32_t addr) {
    flash_cs_force(0);
    addr |= cmd << 24;
    for (int i = 0; i < 4; ++i) {
        ssi->dr0 = addr >> 24;
        addr <<= 8;
    }
}

// Poll the flash status register until the busy bit (LSB) clears
static void __no_inline_not_in_flash_func(flash_wait_ready)(void) {
    uint8_t stat;
    do {
        _flash_do_cmd(FLASHCMD_READ_STATUS, NULL, &stat, 1);
    } while (stat & 0x1 && !flash_was_aborted());
}

// Set the WEL bit (needed before any program/erase operation)
static void __no_inline_not_in_flash_func(flash_enable_write)(void) {
    _flash_do_cmd(FLASHCMD_WRITE_ENABLE, NULL, NULL, 0);
}

static void __no_inline_not_in_flash_func(pico_program_bulk)(uint32_t flash_address, backing_store_int_t *values, size_t item_count) {
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         flash_exit_xip         = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn      flash_flush_cache      = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);

    static backing_store_int_t bulk_write_buffer[WEAR_LEVELING_RP2040_FLASH_BULK_COUNT];

    while (item_count) {
        size_t batch_size = MIN(item_count, WEAR_LEVELING_RP2040_FLASH_BULK_COUNT);
        bool   erased     = true;
        for (size_t i = 0; i < batch_size; i++, values++, item_count--) {
            bulk_write_buffer[i] = ~(*values);
            erased &= bulk_write_buffer[i] == (backing_store_int_t)~0;
        }
        if (erased) {
            // Programming all ones changes nothing, so skip it: consolidated data is mostly zeros.
            flash_address += batch_size * sizeof(backing_store_int_t);
            continue;
        }
        __compiler_memory_barrier();

        connect_internal_flash();
        flash_exit_xip();
        flash_enable_write();

        flash_put_cmd_addr(FLASHCMD_PAGE_PROGRAM, flash_address);
        flash_put_get((uint8_t *)bulk_write_buffer, NULL, batch_size * sizeof(backing_store_int_t), 4);
        flash_wait_ready();
        flash_address += batch_size * sizeof(backing_store_int_t);

        flash_flush_cache();
        flash_enable_xip_via_boot2();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// QMK Wear-Leveling Backing Store implementation

static int  interrupts;
static bool flash_present;

// Erase in 64 KB blocks when the store is aligned to them, otherwise in 4 KB sectors.
#if (WEAR_LEVELING_RP2040_FLASH_BASE) % (1 << 16) == 0 && (WEAR_LEVELING_BACKING_SIZE) % (1 << 16) == 0
#    define ERASE_UNIT (1 << 16)
#else
#    define ERASE_UNIT (FLASH_SECTOR_SIZE)
#endif

// The SDK wrappers assume PICO_FLASH_SIZE_BYTES; use the ROM routines so the
// backing store may sit anywhere on the actual die.
static void __no_inline_not_in_flash_func(pico_erase)(uint32_t flash_address, uint32_t length) {
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         flash_exit_xip         = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      flash_range_erase      = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_flush_cache_fn      flash_flush_cache      = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();
    flash_range_erase(flash_address, length, 1 << 16, 0xD8);
    flash_flush_cache();
    flash_enable_xip_via_boot2();
}

// Whether one erase unit reads back fully erased, uncached.
static bool block_erased(uint32_t block) {
    const volatile uint32_t *p = (const volatile uint32_t *)((XIP_NOCACHE_NOALLOC_BASE) + block);
    for (size_t i = 0; i < ERASE_UNIT / sizeof(uint32_t); ++i) {
        if (p[i] != 0xFFFFFFFF) return false;
    }
    return true;
}

// After each unit erased and read back: a board whose hardware watchdog runs
// may feed it here (a consolidation erases the whole store, several seconds).
__attribute__((weak)) void wear_leveling_rp2040_erased_unit(void) {}

// Erases one unit at a time, skipping units already erased, with interrupts
// restored in between: USB keeps being serviced, and a fresh board's first-boot
// format erases nothing. Fails if a unit doesn't read back erased.
static bool erase_blocks(uint32_t flash_address, uint32_t length) {
    for (uint32_t block = flash_address; block < flash_address + length; block += ERASE_UNIT) {
        if (block_erased(block)) continue;
        interrupts = save_and_disable_interrupts();
        pico_erase(block, ERASE_UNIT);
        restore_interrupts(interrupts);
        if (!block_erased(block)) return false;
        wear_leveling_rp2040_erased_unit();
    }
    return true;
}

#ifndef WEAR_LEVELING_RP2040_PROBE_ATTEMPTS
#    define WEAR_LEVELING_RP2040_PROBE_ATTEMPTS 5
#endif

bool backing_store_init(void) {
    bs_dprintf("Init\n");
    memcpy(BOOT2_ROM_RAM, BOOT2_ROM, sizeof(BOOT2_ROM));
    __compiler_memory_barrier();

#if (WEAR_LEVELING_RP2040_FLASH_BASE) + (WEAR_LEVELING_BACKING_SIZE) * (WEAR_LEVELING_COPIES) > (PICO_FLASH_SIZE_BYTES)
    // Beyond the configured flash size, refuse a die too small for the backing
    // store: addresses past its end would wrap onto the firmware. Retry the
    // probe, since one bad read would leave nothing saved until the next boot.
    flash_present = false;
    for (int attempt = 0; !flash_present && attempt < (WEAR_LEVELING_RP2040_PROBE_ATTEMPTS); ++attempt) {
        uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
        interrupts    = save_and_disable_interrupts();
        flash_do_cmd(tx, rx, sizeof(tx));
        restore_interrupts(interrupts);
        flash_present = rx[3] >= 16 && rx[3] < 32 && (WEAR_LEVELING_RP2040_FLASH_BASE) + (WEAR_LEVELING_BACKING_SIZE) * (WEAR_LEVELING_COPIES) <= (1u << rx[3]);
    }
#else
    flash_present = true;
#endif
    return flash_present;
}

bool backing_store_unlock(void) {
    bs_dprintf("Unlock\n");
    return true;
}

bool backing_store_erase(void) {
#ifdef WEAR_LEVELING_DEBUG_OUTPUT
    uint32_t start = timer_read32();
#endif

    // Ensure the backing size can be cleanly subtracted from the flash size without alignment issues.
    STATIC_ASSERT((WEAR_LEVELING_BACKING_SIZE) % (FLASH_SECTOR_SIZE) == 0, "Backing size must be a multiple of FLASH_SECTOR_SIZE");

    if (!flash_present) return false;
    bool ok = erase_blocks((WEAR_LEVELING_RP2040_FLASH_BASE), (WEAR_LEVELING_BACKING_SIZE) * (WEAR_LEVELING_COPIES));

    bs_dprintf("Backing store erase took %ldms to complete\n", ((long)(timer_read32() - start)));
    return ok;
}

bool backing_store_erase_range(uint32_t address, uint32_t length) {
    if (!flash_present || address % ERASE_UNIT || length % ERASE_UNIT || address + length > (WEAR_LEVELING_BACKING_SIZE) * (WEAR_LEVELING_COPIES)) return false;
    return erase_blocks((WEAR_LEVELING_RP2040_FLASH_BASE) + address, length);
}

bool backing_store_write(uint32_t address, backing_store_int_t value) {
    return backing_store_write_bulk(address, &value, 1);
}

bool backing_store_write_bulk(uint32_t address, backing_store_int_t *values, size_t item_count) {
    // Never program outside the store: past its end lies other flash (the updater's staging slot).
    const uint32_t store_size = (WEAR_LEVELING_BACKING_SIZE) * (WEAR_LEVELING_COPIES);
    if (!flash_present || address > store_size || item_count > (store_size - address) / sizeof(backing_store_int_t)) return false;
    uint32_t offset = (WEAR_LEVELING_RP2040_FLASH_BASE) + address;
    bs_dprintf("Write ");
    wl_dump(offset, values, sizeof(backing_store_int_t) * item_count);
    interrupts = save_and_disable_interrupts();
    pico_program_bulk(offset, values, item_count);
    restore_interrupts(interrupts);

    // Programming can only clear bits, and reports nothing; read back uncached to
    // confirm the values landed.
    const volatile backing_store_int_t *loc = (const volatile backing_store_int_t *)((XIP_NOCACHE_NOALLOC_BASE) + offset);
    for (size_t i = 0; i < item_count; ++i) {
        if (loc[i] != (backing_store_int_t)~values[i]) return false;
    }
    return true;
}

bool backing_store_lock(void) {
    return true;
}

bool backing_store_read(uint32_t address, backing_store_int_t *value) {
    return backing_store_read_bulk(address, value, 1);
}

bool backing_store_read_bulk(uint32_t address, backing_store_int_t *values, size_t item_count) {
    if (!flash_present) return false;
    // Uncached, so that a retried read really reads the flash again.
    uint32_t                      offset = (WEAR_LEVELING_RP2040_FLASH_BASE) + address;
    const volatile backing_store_int_t *loc    = (const volatile backing_store_int_t *)((XIP_NOCACHE_NOALLOC_BASE) + offset);
    for (size_t i = 0; i < item_count; ++i) {
        values[i] = ~loc[i];
    }
    bs_dprintf("Read  ");
    wl_dump(offset, values, item_count * sizeof(backing_store_int_t));
    return true;
}
