// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Pair UF2 (docs/pair-uf2.md): one .uf2 that installs both halves.
//
// The file holds the firmware image (ordinary UF2 blocks from flash offset 0)
// and one more 4 KiB sector, the pair record, at UPDATE_PAIR_RECORD_OFFSET.
// Copied onto RPI-RP2, the boot ROM writes both. The record asks the half it
// lands on to send its own image, which the record's manifest describes, to
// the other half over the split cable at the next boot (the boot relay,
// update_pair_task). Keybard reads the same record out of the file for the
// manifest and installs through the unchanged updater protocol instead.
//
// Who writes the record sector:
//   - the boot ROM, from a UF2 (page 0, the record, and the other 15 pages);
//   - this file's boot relay, which only erases the whole sector and clears
//     bits in page 1 (the state page: its boot count and the other half's
//     fingerprint). It never programs page 0, so firmware can never create a
//     record: only a UF2 copy in BOOTSEL can.
// Nothing a host can reach writes it: the updater's staging writes only its
// slot and the commit only [0, SVAL_UPDATE_MAX_IMAGE) (both checked again
// next to every boot ROM call); settings, identity, the side records and the
// build reset record are their own fixed sectors. update_flash.c asserts the
// layout.

#include <stdbool.h>
#include <stdint.h>
#include "update_manifest.h"
#include "update_image.h"

// The record sector: after the settings (0x160000-0x25FFFF) with a 64 KiB
// gap, below the staging slot (0x800000) and the high records (0xFFB000 up),
// and inside the first 4 MiB of the die.
#define UPDATE_PAIR_RECORD_OFFSET 0x270000u
#define UPDATE_PAIR_SECTOR_BYTES 4096u
#define UPDATE_PAIR_PAGE_BYTES 256u
#define UPDATE_PAIR_STATE_OFFSET 0x100u // page 1 of the sector

#define UPDATE_PAIR_MAGIC 0x55505653u // "SVPU" as bytes in flash
#define UPDATE_PAIR_RECORD_VERSION 1
#define UPDATE_PAIR_KIND_RELAY_SELF 1 // send this image (at flash offset 0) to the other half
#define UPDATE_PAIR_RECORD_BYTES 128

typedef struct __attribute__((packed)) {
    uint32_t magic;        // UPDATE_PAIR_MAGIC
    uint16_t version;      // UPDATE_PAIR_RECORD_VERSION
    uint16_t length;       // UPDATE_PAIR_RECORD_BYTES
    uint8_t  kind;         // UPDATE_PAIR_KIND_RELAY_SELF
    uint8_t  reserved[3];  // 0
    uint32_t image_offset; // 0: the image is the one at the start of flash
    uint8_t  manifest[UPDATE_MANIFEST_BYTES]; // the image's .svup manifest
    uint32_t crc;          // CRC-32/MPEG-2 over the 124 bytes before it
} update_pair_record_t;
_Static_assert(sizeof(update_pair_record_t) == UPDATE_PAIR_RECORD_BYTES, "the pair record is 128 bytes");
_Static_assert(__builtin_offsetof(update_pair_record_t, manifest) == 16, "pair record layout");

// Page 1: written by the boot relay only, a bit at a time (NOR programming
// clears bits; the boot ROM leaves the page erased).
#define UPDATE_PAIR_BOOT_TRIES 3 // boots of the half with USB that may try
typedef struct __attribute__((packed)) {
    uint8_t  tries[UPDATE_PAIR_BOOT_TRIES]; // 0xFF unused; 0x00: a boot began trying
    uint8_t  seen;                          // 0x00 once the other half answered: its fingerprint follows
    uint32_t peer_fw_version;               // from its presence, at the first answer
    uint32_t peer_git_hash;
    uint8_t  peer_proto;
} update_pair_state_t;

// The record names a manifest: what the boot relay and Keybard install.
bool update_pair_record_present(const update_pair_record_t *r); // the magic only
// The record's own fields, then its manifest against this build (dev: floors;
// the manifest must be an image for either hand, UPDATE_HAND_EITHER).
// UPDATE_OK or a refusal (INVALID, UNSUPPORTED for another record version,
// or the manifest check's status).
update_status_t update_pair_record_check(const update_pair_record_t *r, const update_device_t *dev);
void            update_pair_record_make(update_pair_record_t *r, const uint8_t manifest[UPDATE_MANIFEST_BYTES]);

// ---- the boot relay ------------------------------------------------------------------

typedef enum {
    PAIR_BOOT_INIT = 0, // not looked yet (until the first updater_task pass)
    PAIR_BOOT_IDLE,     // nothing to do, or done
    PAIR_BOOT_HASH,     // hashing this half's image against the record's manifest
    PAIR_BOOT_WAIT,     // waiting for the other half's presence
    PAIR_BOOT_RELAY,    // the relay (update_split.c) runs
    PAIR_BOOT_RETRY,    // a link failure: a pause before the next try in this boot
    PAIR_BOOT_ERROR,    // the red light (refused, damaged, tried too often), for SVAL_UPDATE_PAIR_ERROR_MS, then IDLE
} pair_boot_phase_t;

// How the last record ended (the host tests read it).
typedef enum {
    PAIR_OUT_NONE = 0,  // no record
    PAIR_OUT_DONE,      // installed on the other half: erased
    PAIR_OUT_REFUSED,   // the record or the other half refused the image: erased, red light
    PAIR_OUT_STALE,     // not the image this half runs, or the other half changed since: erased, no light
    PAIR_OUT_ABANDONED, // this half booted without USB: erased
    PAIR_OUT_EXHAUSTED, // UPDATE_PAIR_BOOT_TRIES boots tried: erased, red light
    PAIR_OUT_KEPT,      // no other half that can take it this boot: kept for the next, no light
} pair_boot_outcome_t;

void            update_pair_task(void); // from updater_task, every pass, on either half
bool            update_pair_busy(void); // a record is being worked on: host session ops get BUSY
uint8_t         update_pair_phase(void);   // pair_boot_phase_t
bool            update_pair_peer_takes(void); // the other half answers presence and runs a relay's other end (the lights)
uint8_t         update_pair_outcome(void); // pair_boot_outcome_t
update_status_t update_pair_error(void);   // the status behind REFUSED or KEPT
#ifdef SVAL_UPDATER_HOST_TEST
void update_pair_host_reset(void); // this half's RAM after a reset
#endif
