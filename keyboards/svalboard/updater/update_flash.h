// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Staging-slot flash access for the updater. Everything here erases or programs
// only inside [SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE): offsets
// are relative to the slot, and every flash operation re-checks its absolute
// range before touching the die. The commit (update_commit.c) has its own RAM
// routines and does not use these.

#include <stdbool.h>
#include <stdint.h>
#include "update_manifest.h"

#define UPDATE_FLASH_PAGE 256u
#define UPDATE_FLASH_SECTOR 4096u
#define UPDATE_FLASH_BLOCK 65536u
#define UPDATE_JEDEC_BYTES 3

void update_flash_init(void);      // reads the JEDEC ID once; safe to call repeatedly
bool update_flash_available(void); // the die is 16 MiB, so the slot exists (D26)
void update_flash_jedec(uint8_t id[UPDATE_JEDEC_BYTES]); // manufacturer, type, capacity

// Whether [off, off + len) lies inside the slot (no wrap-around).
bool update_slot_range_ok(uint32_t off, uint32_t len);

// An uncached view of the slot: reads see the die, never stale XIP cache lines.
// NULL when [off, off + len) is outside the slot.
const volatile uint8_t *update_slot_read(uint32_t off, uint32_t len);

// Whether one 64 KiB block / 4 KiB sector of the slot (off aligned to it)
// reads all 0xFF.
bool update_flash_block_erased(uint32_t off);
bool update_flash_sector_erased(uint32_t off);

// Erases one 64 KiB block of the slot (off aligned to it) unless it already
// reads erased, then checks it reads back erased. Interrupts are off for the
// erase itself. UPDATE_OK, UPDATE_INVALID (range or alignment),
// UPDATE_UNAVAILABLE (die not 16 MiB) or UPDATE_FLASH_ERR.
update_status_t update_flash_erase_block(uint32_t off);

// The same for one 4 KiB sector (off aligned to it). The updater stages with
// these: a sector erase keeps interrupts off for about 45 ms (W25Q128JV tSE
// typical; 400 ms max) against 150 ms (2 s max) for a block, so no host op
// waits on a whole block erase.
update_status_t update_flash_erase_sector(uint32_t off);

// Programs one 256 B page of the slot (off aligned to it) from data, which may
// be anywhere; it is copied to RAM first. The page must read erased beforehand
// (UPDATE_INVALID otherwise: NOR can only clear bits) and must read back equal
// afterwards (UPDATE_FLASH_ERR otherwise).
update_status_t update_flash_program_page(uint32_t off, const uint8_t data[UPDATE_FLASH_PAGE]);

// The pair record sector (update_pair.h, docs/pair-uf2.md), at the fixed
// offset UPDATE_PAIR_RECORD_OFFSET: an uncached view of it, its erase, and a
// program of its state page (page 1) over what is there (NOR: bits are only
// cleared; the result must read back as old AND new). The RAM routine takes
// no address: these can reach that sector and nothing else, and never page 0,
// which only a UF2 copy in BOOTSEL writes. UPDATE_OK, UPDATE_UNAVAILABLE (die
// not 16 MiB) or UPDATE_FLASH_ERR.
const volatile uint8_t *update_pair_sector_read(void);
update_status_t         update_pair_sector_erase(void);
update_status_t         update_pair_state_program(const uint8_t page[UPDATE_FLASH_PAGE]);
