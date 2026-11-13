// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The settings area: every keyboard setting, as labeled records in a fixed
// block of the stored settings (SVAL_SETTINGS_AREA_ADDR, SVAL_SETTINGS_AREA_SIZE
// bytes; modules/svalboard/core/post_config.h). The 100-byte VIA custom config
// is no longer read or written.
//
// Layout (little-endian):
//   header, 8 bytes: 'S' 'V' 'S' 'A', format 1, 0, area size (u16)
//   entries, back to back:
//     id u16, capacity u16, then two slots of
//       seq u8, len u16, payload[capacity], crc8 (over id, seq, len, payload)
//   an id of 0x0000 ends the list (the erased store reads 0).
//
// A save writes the slot that does not hold the current value, with the next
// sequence number; its CRC is written last. A power cut during a save leaves
// that slot failing its CRC, so the board reads the previous value: every
// setting reads as its old or its new value, never a mix. A record with no
// valid slot, or none at all, reads as missing: the caller uses its default.
// Unknown ids (from newer firmware) are skipped by their capacity.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings_ids.h"

#define SVAL_SETTINGS_HEADER_BYTES 8
#define SVAL_SETTINGS_ENTRY_HEAD_BYTES 4
#define SVAL_SETTINGS_SLOT_HEAD_BYTES 4 // seq, len (2), crc
#define SVAL_SETTINGS_ENTRY_BYTES(cap) (SVAL_SETTINGS_ENTRY_HEAD_BYTES + 2 * (SVAL_SETTINGS_SLOT_HEAD_BYTES + (cap)))
#define SVAL_SETTINGS_RETIRED_ID 0xFFFEu
#define SVAL_SETTINGS_FORMAT 1

#define SVAL_SETTINGS_SUM_(name, id, cap) +SVAL_SETTINGS_ENTRY_BYTES(cap)
// Bytes every record takes at its full capacity, with the header and end mark.
#define SVAL_SETTINGS_USED_BYTES (SVAL_SETTINGS_HEADER_BYTES + 2 SVAL_SETTINGS_RECORDS(SVAL_SETTINGS_SUM_))

// Storage the area lives in: offsets from the area's start.
typedef struct {
    void (*read)(uint16_t offset, void *buf, uint16_t len);
    void (*write)(uint16_t offset, const void *buf, uint16_t len); // writes only changed bytes
    uint16_t size;
} settings_area_store_t;

void settings_area_use(const settings_area_store_t *store);
// Whether the header is this format's. False on a blank or foreign area.
bool settings_area_valid(void);
// Write a fresh header and an empty list. Every record then reads as missing.
void settings_area_format(void);
// Copy up to max bytes of the record's current value into buf. Returns false
// when there is none; *len (if given) gets the stored length, which may be
// shorter than max (older firmware) or longer (newer firmware: cut to max).
bool settings_area_get(uint16_t id, void *buf, uint16_t max, uint16_t *len);
// Store a value. Does nothing when it is already stored. Returns false if the
// area has no room for a new record.
bool settings_area_put(uint16_t id, const void *buf, uint16_t len);
// The capacity the build gives an id, 0 if unknown.
uint16_t settings_area_capacity(uint16_t id);
