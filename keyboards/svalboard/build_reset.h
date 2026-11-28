// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Stored settings reset on the first boot of a different firmware build (D39).
//
// At boot the firmware hashes its own image in flash and compares the hash with
// the record in BUILD_RESET_SECTOR. When they differ, or there is no valid
// record (Vial, vLaunch2 and older Svalboard QMK never write one), every stored
// setting is reset to this build's defaults, through the same path as a new
// EECONFIG_MAGIC_NUMBER; once the board is up the hash is recorded, with a
// notice for the host (GET_INFO byte 18 bit 2) until it sends 0x2A. Flashing
// the identical image again finds its own hash and keeps the settings.
//
// The sector lies outside the settings and below the identity sectors, so a
// settings reset never erases the record. On a die smaller than 16 MB the
// sector does not exist: there is nowhere to keep the record, so the build
// keeps settings (an incompatible store is still reset by the EECONFIG magic
// and the Sval storage stamp). When the record cannot be written, settings
// reset again at the next boot, never mid-session, and GET_INFO byte 18 bit 1
// tells the host that a change could not be saved.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUILD_RESET_SECTOR 0xFFD000u  // the 4 KB sector below IDENTITY_SECTOR_A (identity.h)
#define BUILD_RESET_MAGIC 0x52425653u // "SVBR"
#define BUILD_RESET_NOTICE 0x01u      // the host has not acknowledged the reset yet

typedef struct __attribute__((packed)) {
    uint32_t magic; // BUILD_RESET_MAGIC
    uint32_t image; // hash of the image the settings were last reset for
    uint32_t flags; // BUILD_RESET_NOTICE
    uint32_t check; // ~(magic ^ image ^ flags)
} build_reset_record_t;

// FNV-1a over the image, 32-bit words; a trailing partial word is padded with 0xFF.
uint32_t build_reset_hash(const uint8_t *image, size_t len);
// Whether rec (NULL: none) holds a valid record for image.
bool build_reset_matches(const build_reset_record_t *rec, uint32_t image);
// Whether a boot of image must reset settings. available: the sector exists.
bool build_reset_due(bool available, const build_reset_record_t *rec, uint32_t image);
// Whether rec says the host has yet to acknowledge the reset for image.
bool build_reset_notice(const build_reset_record_t *rec, uint32_t image);
void build_reset_record_make(build_reset_record_t *rec, uint32_t image, uint32_t flags);

// SETTINGS_RESET (D42): the page programmed over the record's first page to
// invalidate it without an erase. NOR programming only clears bits, so the
// record's bytes become 0 whatever they were (no valid record has magic 0),
// and the rest of the page (0xFF) is left as it is.
//
// The 8 bytes after the record are the blank mark (D44). SETTINGS_RESET mode
// BLANK programs BUILD_RESET_BLANK_MAGIC and its complement there, so the next
// boot resets every stored setting and then sets every key on every layer to
// KC_NO. Mode DEFAULTS programs zeros there, so a mark left by a blank reset
// that never completed cannot turn a reset to defaults into a blank one. The
// mark counts only over a record that is not valid (cleared, or a record write
// cut short), and a mark programmed over bytes that were not erased reads
// wrong, so build_reset_invalidate() then fails.
//
// The sector is a log of BUILD_RESET_SLOTS one-page slots, so that no erase
// stands between a request and the record that answers it. The slot in force
// is the last of the leading slots whose record bytes are not erased (slot 0
// alone for firmware from before the log, which erases the sector for every
// record). A slot holds the record, the blank mark, then the acknowledgement
// mark:
//   - the host's acknowledgement (0x2A) programs BUILD_RESET_ACK_MAGIC and its
//     complement into the slot in force. The record itself is never rewritten,
//     so a power cut there never resets the settings.
//   - the record after a blank reset goes into the next free slot, its blank
//     mark first, so the request stays in force until the record reads back;
//     then the marks of both slots are zeroed.
//   - every other record erases the sector and starts again at slot 0, as
//     before: a power cut there only repeats the reset that just happened.
// Only a blank reset that finds the slots used up still erases first (the
// last slot stays free for a SETTINGS_RESET after it).
#define BUILD_RESET_PAGE_BYTES 256u
#define BUILD_RESET_BLANK_MAGIC 0x4B4E4C42u // "BLNK"
#define BUILD_RESET_HEAD_BYTES 24u          // the record, then the blank mark (magic, ~magic)
#define BUILD_RESET_TO_DEFAULTS 0u          // SETTINGS_RESET mode 0: firmware defaults
#define BUILD_RESET_TO_BLANK 1u             // SETTINGS_RESET mode 1: defaults, then every key KC_NO
#define BUILD_RESET_MODE_MAX BUILD_RESET_TO_BLANK
#define BUILD_RESET_SLOTS 16u             // one-page slots in the 4 KB sector
#define BUILD_RESET_ACK_MAGIC 0x4B434153u // "SACK": the host acknowledged the notice
#define BUILD_RESET_ACK_OFFSET 24u        // in a slot: after the record and the blank mark
void build_reset_invalid_page(uint8_t page[BUILD_RESET_PAGE_BYTES], uint8_t mode);
// What the sector's first BUILD_RESET_HEAD_BYTES read after that page is programmed over them.
void build_reset_program_head(uint8_t head[BUILD_RESET_HEAD_BYTES], const uint8_t page[BUILD_RESET_PAGE_BYTES]);
// A slot's head asks for a blank board: a record that is not valid and a whole blank mark.
bool build_reset_blank_requested(const uint8_t head[BUILD_RESET_HEAD_BYTES]);
// What the record reads after that page is programmed over it (bitwise AND).
void build_reset_program_over(build_reset_record_t *rec, const uint8_t page[BUILD_RESET_PAGE_BYTES]);
// The record reads all zeros: invalidated by SETTINGS_RESET (never a valid
// record, never erased flash).
bool build_reset_cleared(const build_reset_record_t *rec);

// The log (above). sector: the whole sector as it reads.
// The slot in force; -1 when there is none (an erased sector).
int build_reset_slot(const uint8_t *sector);
typedef struct {
    bool pending;   // this boot resets every stored setting
    bool notice;    // not pending, and the host has yet to acknowledge the reset
    bool blank_due; // pending, and the reset is to a blank board (D44)
    bool requested; // SETTINGS_RESET asked for a reset (dev keep builds reset only then)
} build_reset_state_t;
void build_reset_state(const uint8_t *sector, uint32_t image, build_reset_state_t *st);
// The flash under the log. The log fills page before each program(), which
// programs it at off in the sector, with no erase. rewrite() erases the
// sector, then programs data at its start; true if it reads back.
typedef struct {
    uint8_t *page; // BUILD_RESET_PAGE_BYTES
    void (*program)(uint32_t off);
    bool (*rewrite)(const void *data, uint32_t len);
} build_reset_flash_t;
// The record for image after a reset (flags: BUILD_RESET_NOTICE). blank: the
// reset was to a blank board. True if the record reads back.
bool build_reset_log_record(const uint8_t *sector, const build_reset_flash_t *f, uint32_t image, uint32_t flags, bool blank);
// The host acknowledged the notice (0x2A). True if it now reads acknowledged.
bool build_reset_log_ack(const uint8_t *sector, const build_reset_flash_t *f);
// SETTINGS_RESET: the invalid page over the slot in force, or for a blank
// reset into the next free slot when that slot's blank mark bytes are no
// longer erased. True if the slot in force then reads invalid, with the mark
// as mode asks.
bool build_reset_log_invalidate(const uint8_t *sector, const build_reset_flash_t *f, uint8_t mode);

#ifndef BUILD_RESET_HOST_TEST
// SETTINGS_RESET (D42): invalidates the record with one page program (no
// erase: under 1 ms, so the half without USB can do it in the SlaveThread),
// so the next boot resets every stored setting as for a different build and
// sets the notice. Interrupts are off for the program. Until the reset, 0x2A
// leaves the record alone. False when the sector does not exist or the record
// still reads valid.
// Mode BUILD_RESET_TO_BLANK also writes the blank mark (D44): after the reset
// to defaults, every key on every layer becomes KC_NO. Also false when the
// mark does not read back as asked.
bool build_reset_invalidate(uint8_t mode);
void build_reset_pre_init(void);  // keyboard_pre_init_kb: marks stored settings invalid when due
void build_reset_post_init(void); // keyboard_post_init_kb: records the image after the reset
bool build_reset_notice_pending(void);
bool build_reset_record_failed(void);
bool build_reset_blanked(void); // this boot set every key to KC_NO (D44)
void build_reset_notice_clear(void); // Sval 0x2A
#endif
