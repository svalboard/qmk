// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Board identity: persistent USB serial and user-chosen name (identity.c).

#define IDENTITY_SERIAL_BYTES 8
#define IDENTITY_NAME_MAX_BYTES 64 // UTF-8; the app limits names to 32 characters

// The last two 4 KB sectors of the 16 MB die (flash offsets). The updater's
// staging slot must end below IDENTITY_SECTOR_A (updater/update_flash.c).
#define IDENTITY_SECTOR_A 0xFFE000u
#define IDENTITY_SECTOR_B 0xFFF000u

typedef enum {
    IDENTITY_SERIAL_NONE     = 0,
    IDENTITY_SERIAL_FLASH_ID = 1, // the flash die's unique ID
    IDENTITY_SERIAL_RANDOM   = 2, // the die has no ID; 64 random bits, stored
} identity_serial_source_t;

typedef enum {
    IDENTITY_OK           = 0,
    IDENTITY_INVALID      = 1, // too long, or not valid UTF-8 text
    IDENTITY_UNAVAILABLE  = 2, // the flash is not 16 MB, so there is nowhere to keep it
    IDENTITY_WRITE_FAILED = 3,
} identity_status_t;

// No identity flags are defined. Bits 0x01 and 0x02 were set by a Vial migration
// that release candidates vRC1 and vRC2 ran; boards keep them, so never reuse them.

void              identity_init(void); // safe to call repeatedly; loads once
bool              identity_available(void);
const uint8_t    *identity_serial(identity_serial_source_t *source);
const char       *identity_name(void); // "" when unset
identity_status_t identity_set_name(const char *name, uint8_t len);

// Host access over VIA custom values (protocol in identity.c).
#define IDENTITY_CHANNEL 0x49 // 'I'
enum { IDENTITY_OP_INFO = 0, IDENTITY_OP_NAME = 1, IDENTITY_OP_COMMIT = 2, IDENTITY_OP_REBOOT = 3 };
void identity_via_command(uint8_t *data, uint8_t length);
void identity_task(void); // from housekeeping: carries out a requested reboot

// The build reset record (build_reset.c): writes len bytes at the start of
// BUILD_RESET_SECTOR, erasing it first. False if the region is missing or the
// bytes did not read back.
bool identity_build_record_write(const void *data, uint32_t len);

#ifdef SVAL_EITHER_HAND
// The side record (side.c): writes len bytes at the start of one of the two
// side sectors, erasing it first. False for any other sector, if the region is
// missing, or if the bytes did not read back.
bool identity_side_record_write(uint32_t sector, const void *data, uint32_t len);
#endif
