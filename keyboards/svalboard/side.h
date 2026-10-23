// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Which half this is, in the one-image ("either hand") builds (SVAL_EITHER_HAND).
//
// The per-hand builds fix the side at compile time (INIT_EE_HANDS_LEFT/RIGHT,
// written into the EEPROM handedness byte on every boot). An either build runs
// the same image on both halves and decides the side at boot, in this order:
//
//   1. The thumb strap (svalboard.c): a left thumb with R7 to GND, or a right
//      thumb with R7 to the thumb row. A strap always wins, and the stored
//      record is rewritten to match it (source STRAP).
//   2. The stored side record, below.
//   3. Unknown. The board enumerates and answers raw HID, but types nothing
//      (side.c); a host sets the side with Sval 0x33 (docs/protocol.md).
//
// The record lives in its own two 4 KB flash sectors, below the build reset
// sector and above the updater's staging slot. Settings resets (the build
// reset, a new EECONFIG magic, EE_CLR, VIA and Sval resets), storage format
// changes, UF2 copies and in-firmware updates never touch them; only a
// whole-chip erase does. The two sectors hold alternating copies with a
// sequence number and a CRC, so a power cut during a write leaves the older
// copy. Only an explicit host write or a thumb strap changes the record.
//
// The first boot of an either build on a board with no record at all adopts
// the side the per-hand firmware left in the EEPROM handedness byte (source
// MIGRATED), before the build reset formats the settings. With nothing to
// adopt it writes an UNKNOWN record, so a later boot never mistakes the
// formatted settings for a side.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVAL_SIDE_SECTOR_A 0xFFB000u // the two 4 KB sectors below BUILD_RESET_SECTOR (0xFFD000)
#define SVAL_SIDE_SECTOR_B 0xFFC000u
#define SVAL_SIDE_MAGIC 0x44535653u // "SVSD"
#define SVAL_SIDE_VERSION 1

// Sides, numbered as the updater's hands (UPDATE_HAND_LEFT/RIGHT).
#define SVAL_SIDE_LEFT 0
#define SVAL_SIDE_RIGHT 1
#define SVAL_SIDE_UNKNOWN 0xFF

// Where a side came from.
enum sval_side_source {
    SVAL_SIDE_SRC_NONE     = 0, // nothing: no strap, no record, nothing to migrate
    SVAL_SIDE_SRC_FACTORY  = 1, // a host write from the factory tool
    SVAL_SIDE_SRC_HOST     = 2, // a host write from Keybard
    SVAL_SIDE_SRC_MIGRATED = 3, // adopted from the EEPROM handedness byte a per-hand build left
    SVAL_SIDE_SRC_STRAP    = 4, // the thumb strap
};

typedef struct __attribute__((packed)) {
    uint32_t magic;   // SVAL_SIDE_MAGIC
    uint8_t  version; // SVAL_SIDE_VERSION
    uint8_t  length;  // sizeof(sval_side_record_t)
    uint8_t  side;    // SVAL_SIDE_*
    uint8_t  source;  // enum sval_side_source
    uint32_t seq;     // the newer copy has the larger number
    uint32_t crc;     // CRC-32 (IEEE) over the bytes before it
} sval_side_record_t;

// EEPROM magics whose handedness byte (offset 14) a per-hand Svalboard build
// wrote: 0xFEE2 (svalboard/qmk), 0xB1A0 (SVAL_TEST_WIPE bench builds), 0xFEE3
// (QMK's own, used before 0xFEE2, same wear-leveling region).
#define SVAL_SIDE_EECONFIG_HANDEDNESS 14

// The Vial firmware (svalboard/vial-qmk, per-hand builds) kept its settings in
// QMK wear leveling built for a 2 MB chip: 128 KB of backing at 0x1E0000, 64 KB
// logical, 2-byte backing writes, every backing word inverted in flash. Its
// handedness byte is at offset 14, as above, with QMK's magic 0xFEE3.
#define SVAL_SIDE_VIAL_STORE_BASE 0x1E0000u
#define SVAL_SIDE_VIAL_STORE_BACKING (128u * 1024u)
#define SVAL_SIDE_VIAL_STORE_LOGICAL (64u * 1024u)
// The backing word at byte offset off of that store, already un-inverted.
typedef uint16_t (*sval_side_word_fn)(uint32_t off, void *ctx);
// The side a Vial store recorded: QMK's boot replay (the consolidated image if
// its FNV-1a 64 checksum matches, then the write log) for the first 16 bytes only.
uint8_t sval_side_from_vial_store(sval_side_word_fn word, void *ctx);

uint32_t sval_side_crc32(const uint8_t *p, size_t n);
bool     sval_side_record_valid(const sval_side_record_t *r);
void     sval_side_record_make(sval_side_record_t *r, uint8_t side, uint8_t source, uint32_t seq);
// The newer valid copy, or NULL. *write_b: which copy a save should replace (the older or invalid one).
const sval_side_record_t *sval_side_newest(const sval_side_record_t *a, const sval_side_record_t *b, bool *write_b);
// The side a per-hand build left in the EEPROM, or SVAL_SIDE_UNKNOWN.
uint8_t sval_side_from_eeconfig(uint16_t magic, uint8_t handedness);
// The strap's side (enum sval_thumb_hand), or SVAL_SIDE_UNKNOWN.
uint8_t sval_side_from_strap(uint8_t strap_class);

typedef struct {
    uint8_t side;   // the side this boot runs as
    uint8_t source; // where it came from
    bool    write;  // a record must be written: write_side, write_source
    uint8_t write_side, write_source;
} sval_side_decision_t;
// Boot-time priority: strap, record (NULL: no valid copy), then migration
// (migrated: sval_side_from_eeconfig, only looked at when there is no record).
void sval_side_decide(uint8_t strap_side, const sval_side_record_t *rec, uint8_t migrated, sval_side_decision_t *d);

// Host write status (Sval 0x33).
enum sval_side_status {
    SVAL_SIDE_OK            = 0,
    SVAL_SIDE_INVALID       = 1, // bad side, source or target
    SVAL_SIDE_UNAVAILABLE   = 2, // the flash is not 16 MB: there is nowhere to keep it
    SVAL_SIDE_WRITE_FAILED  = 3,
    SVAL_SIDE_STRAP         = 4, // the thumb strap says the other side; the strap wins
    SVAL_SIDE_NOT_CONNECTED = 5, // the other half is not connected
    SVAL_SIDE_NO_ANSWER     = 6, // the other half did not answer (older firmware, or the link failed)
};
// Whether a host may write side (SVAL_SIDE_LEFT/RIGHT, or UNKNOWN to clear) with source.
uint8_t sval_side_host_check(uint8_t strap_side, uint8_t side, uint8_t source);

// Pairing: this half's side and the other half's as it reports it.
enum sval_side_pair {
    SVAL_SIDE_PAIR_OK       = 0, // both known and different, or the other half is not connected
    SVAL_SIDE_PAIR_SAME     = 1, // both halves claim the same side
    SVAL_SIDE_PAIR_UNKNOWN  = 2, // a connected half has no side
    SVAL_SIDE_PAIR_PENDING  = 3, // the other half is connected and has not reported yet
};
uint8_t sval_side_pair(uint8_t mine, bool other_connected, bool other_reported, uint8_t other);

#ifndef SVAL_SIDE_HOST_TEST
void    sval_side_init(void); // keyboard_pre_init_kb: after identity_init() and the strap probe, before build_reset_pre_init()
uint8_t sval_side(void);      // SVAL_SIDE_*, for this boot
uint8_t sval_side_source(void);
bool    sval_side_known(void);
void    sval_side_task(void); // housekeeping: input blocking edges, a requested restart
// Input is dropped: this half has no side, or the pair is not sound.
bool sval_side_input_blocked(void);
// Whether keys from this matrix row may act (row in the 10-row split matrix).
bool sval_side_row_allowed(uint8_t row);
// The other half's side from its presence reply: reported (false until it
// answers), and its side (SVAL_SIDE_UNKNOWN when it has none).
void sval_side_other_set(bool connected, bool reported, uint8_t side);
// Sval 0x33 (modules/svalboard/core/sval.c): data after [0xDF][0x33].
void sval_side_command(uint8_t *data, uint8_t length);
// The other half's end of a relayed write (KEYBOARD_SYNC_B, first byte SVAL_SIDE_RPC_OP).
#    define SVAL_SIDE_RPC_OP 0xA5
bool sval_side_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
// The split link pins this half chose (SERIAL_USART_PINS_RUNTIME): its TX GPIO,
// 0xFF before the link started. how: 0 by side, 1 found by listening, 2 by role default,
// 3 swapped by the master's search while the link was down.
uint8_t sval_side_link_tx(uint8_t *how);
#endif
