// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// In-firmware updater: the session state machine (see updater.h).
//
// ---- host protocol: VIA custom values on UPDATE_CHANNEL (0x55, 'U') ----------
//
// Packet: one raw HID report of RAW_EPSIZE bytes (64: keyboards/svalboard/config.h),
// [0xDD][client_id:4][0xFE] + VIA [command][0x55][op][value_data:VALUE_BYTES],
// VALUE_BYTES = RAW_EPSIZE - 9 (55): the wrapper's 6 bytes and VIA's 3. command
// = id_custom_set_value or id_custom_get_value. The reply is the same packet
// with value_data rewritten; value_data[0] is always an update_status_t.
// Every op but INFO needs the client wrapper (UPDATE_INVALID otherwise, with no
// state change), and every request starts with [hand]: this half's
// (UPDATE_HAND_*), or the other half's (M2: see "The other half" below), else
// UPDATE_UNSUPPORTED. INFO also takes hand 0xFF for "this half". Multi-byte
// fields are little-endian.
//
//  op           request after [hand]             reply after [status]
//  0x00 INFO     [page u8: 0, the default]       state, protocol, slot base/4K u16, slot size/4K u16,
//                                                max image/4K u16, JEDEC[3], pointing_id, hand,
//                                                fw_version u32, storage format, security epoch u16,
//                                                flags (bit0 die is 16 MiB, bit1 settings writes failing,
//                                                bit2 release build, bit3 test hooks, bit4 reserved,
//                                                bit5 the other half's version differs: see STATUS,
//                                                bit6 reserved, bit7 a no-chord dev build: every ARM
//                                                counts as confirmed, D43), last error; then (protocol 3) the
//                                                largest CHUNK data u8 (CHUNK_DATA_MAX, 50), the
//                                                largest MANIFEST piece u8 (MANIFEST_DATA_MAX, 52)
//                                                and the raw HID report size u8 (RAW_EPSIZE, 64).
//                                                Status UNAVAILABLE when bit0 is clear or bit1 set.
//                page 1 (M3; this half only)     1, fw_version u32, version string (16 chars, NUL-padded:
//                                                the release tag). Other pages: INVALID.
//  0x01 MANIFEST off u8, n 1..52, bytes          filled u8 (of the 108-byte manifest)
//  0x02 ARM      mode u8 (protocol 4: 0 single,  nonce u32, first 4 bytes of sha512(108 B manifest),
//                1 pair; see "One approval")     first 4 bytes of sha512(the pair manifest) (pair; else 0),
//                                                flags (bit0 PRE_APPROVED: BEGIN needs no chord, bit1
//                                                BY_PAIR, bit2 NO_CHORD, bit3 PAIR; update_manifest.h)
//  0x03 BEGIN    nonce u32                       state
//  0x04 CHUNK    off u24, n 1..50, bytes         next offset u24
//  0x05 END      nonce u32                       state
//  0x06 STATUS   -                               state, staged u24, image_len u24, sectors erased u16,
//                                                sectors to erase u16, last error, reserved u16 (0),
//                                                image verify ms u16, longest sector erase ms u16,
//                                                image CRC low 16 bits (once VERIFIED), flags (bit0
//                                                chord made, bit1 session bound, bit2 settings writes
//                                                failing), the other half by split presence
//                                                (update_presence_status_t; M2, V); flags2 (protocol 4:
//                                                bit0 an approval is held for this half's next install,
//                                                bit1 the current session was pre-approved)
//  0x07 COMMIT   nonce u32, CRC low 16 bits      state
//  0x08 ABORT    nonce u32                       state
//  0x09 REBIND   nonce u32                       state
//  0x0A TEST_HALT nonce u32, point u8, fed u8    state (SVAL_UPDATE_TEST_HOOKS builds only; INVALID
//                                                elsewhere): the halt point for this session's commit
//                                                (update_halt_t, update_commit.h), in VERIFIED only.
//  0x0B DIAG     clear u8                        main's stack: free bytes u16 (never used since boot),
//                                                size u16; longest updater_task() pass ms u16 and the
//                                                state it began in u8. clear 1 restarts the pass
//                                                maximum after replying. Any wrapped client (M1 #13).
//  0x0C RELAY    -                               M2: the relay to the other half (update_split.h): phase
//                                                (relay_phase_t), the other half's state and last error,
//                                                bytes it acknowledged u24, image_len u24, its sectors
//                                                erased u16 and to erase u16, retries u16, relay ms u32,
//                                                flags (bit0 the link is paused; bit1 DONE without an
//                                                answer from the other half after COMMIT, unconfirmed),
//                                                the relay's error, this
//                                                half's session state. Any hand.
//  0x0D PAIR_MANIFEST off u8, n 1..52, bytes     filled u8 (of the 108-byte pair manifest). Protocol 4,
//                [hand] = this half's own        D41: this half's manifest, while a session for the
//                                                other hand is in MANIFEST_LOADING (see "One approval")
//  0x0E SETTINGS_RESET 'R' 'S' 'E' 'T', scope u8 done u8 (bit0 this half, bit1 the other half).
//                (0 this half, 1 both),          Protocol 4, D42: see "Settings reset"
//                mode u8 (0 defaults, 1 blank: D44)
//
// Protocol 3 is protocol 2 in 64-byte reports: the same ops and fields, with
// larger MANIFEST and CHUNK pieces and the three sizes at the end of INFO page
// 0. A host sizes its pieces from INFO. Protocol 4 is protocol 3 plus
// PAIR_MANIFEST, the ARM mode and reply flags, the held approval, STATUS byte
// 23, SETTINGS_RESET and INFO flags bit7; a protocol 3 host that sends ARM
// with no mode byte gets a single ARM, as before.
//
// One approval for both halves (protocol 4, D41). A pair update sends
// MANIFEST(other hand) in pieces, then PAIR_MANIFEST(this hand) with this
// half's own manifest, then ARM(other hand, mode 1). The pair ARM checks the
// other manifest against the other half (as a single ARM does) and the pair
// manifest against this half's build; either refusal latches ERROR, so the
// user is never asked to approve a file one half would refuse. The chord, made
// once in that session, creates the held approval: SHA-512 of the pair
// manifest, the session's client ID and its nonce, in RAM only (on a no-chord
// build it is created at the pair ARM). Once the relay reaches DONE the
// approval lasts SVAL_UPDATE_PAIR_HOLD_MS. This half's own single ARM from the
// bound client whose manifest has that SHA-512 is then pre-approved (flags
// PRE_APPROVED and BY_PAIR, no blue light): BEGIN is ACCEPTED at once. Any
// ARM for this half uses the approval up, matching or not. REBIND with the
// pair session's nonce moves the approval to the sender in any state, IDLE
// included. ABORT, any ERROR, a MANIFEST from another client or for the other
// hand, a reset and the timeout clear it.
//
// Settings reset (protocol 4, D42). SETTINGS_RESET, only in IDLE with no held
// approval and no relay (else BUSY), invalidates the build reset record
// (kb/build_reset.c, D39) of the other half first (scope 1: over
// KEYBOARD_UPDATE; UNAVAILABLE and nothing changed if it does not answer),
// then of this half, replies, and resets this half
// SVAL_UPDATE_SETTINGS_RESET_MS later (the other half resets itself). Each
// half's next boot then resets every stored setting as for a different build,
// keeping its identity, and sets the notice (GET_INFO byte 18 bit 2) until 0x2A.
// Mode 1 (D44) asks both halves for a blank board: the same reset, then every
// key on every layer KC_NO (the blank mark, kb/build_reset.h).
//
// The image's own build info (D45). While VERIFYING_IMAGE hashes an image, a
// release build also reads its build-info records (update_build_scan) and
// refuses a no-chord or kept-settings dev image with BAD_IMAGE, whatever the
// manifest's flags say. The half without USB checks a relayed image again.
//
// The image CRC is CRC-32/MPEG-2 over image bytes [0x100, image_len): the
// range the commit checks after copying (page 0 is written last).
//
// The other half (M2, store and forward, D14). With hand = the other side,
// MANIFEST starts a session for the other half, on this half (the one with
// USB, where the chord is made, D4). ARM needs split presence to answer MATCH
// or VERSION (else UNAVAILABLE) and checks the manifest against this build's
// floors; BEGIN, CHUNK and
// END stage and verify the image in this half's slot exactly as for this half.
// Then:
//   COMMIT in VERIFIED  -> RELAYING: the image goes to the other half from this
//                          slot, which checks the manifest and hash itself (D7)
//   (relay done)        -> RELAYED: the other half holds the image, verified
//   COMMIT in RELAYED   -> SUBSIDE_COMMITTING: the other half commits; the link
//                          stays paused through its reset (R18); then IDLE
// Both COMMITs carry the nonce and the image CRC. ABORT stops a relay up to the
// second COMMIT. A relay failure latches ERROR with its status (UNAVAILABLE:
// the other half stopped answering). While a session for one hand runs, the
// session ops for the other hand get BUSY. INFO with the other hand asks the
// other half (KEYBOARD_UPDATE INFO): the reply has INFO's layout with that
// half's state, protocol, pointing ID, hand, fw version, storage format, epoch
// and flags bits 0-3, no JEDEC ID (zeros) and no last error; with no answer it
// is UNAVAILABLE with state 0xFF (as Scan Lab's relay). The request's hand,
// not the manifest's, picks the half: a session for the other hand never
// reaches COMMITTING here. The manifest's hand and pointing device are not
// checked by either half (D33): an image built for the other side or module
// is taken, and the host tools warn and ask first.
//
// Sessions. MANIFEST from any wrapped client starts loading and ties the load
// to that client. ARM, from the same client with all 108 bytes, checks the
// manifest's fields, binds the session to that client ID and a fresh nonce
// (D3) and waits for the chord. From then on BEGIN, END, COMMIT and ABORT need
// the client ID and the nonce, CHUNK the client ID; another client gets
// OTHER_CLIENT and changes nothing. REBIND moves the session to the sender's
// client ID when the nonce matches. While ERROR is latched anyone wrapped may
// ABORT; ABORT never erases.
//
// The binding keeps well-behaved clients apart; it is not a defence against a
// hostile one (R10). The client ID travels in every wrapped reply and the nonce
// in ARM's, and raw HID replies reach every open handle, so another process
// with the device open can REBIND the session to itself and then stall, ABORT
// or COMMIT it. It still cannot change what is installed: the chord confirms
// the manifest hash, and the manifest's SHA-512 binds the image.
//
// Refusals of a request (INVALID, OTHER_CLIENT, BUSY, OUT_OF_ORDER, a chord not
// made yet, a wrong COMMIT CRC, UNAVAILABLE at ARM/BEGIN/COMMIT, ARM during a
// Scan Lab sweep) change no state. Failures of the update itself latch ERROR with last_error set: a
// manifest check, the image hash or structure, a flash error,
// OVERRUN, the chord window passing (NOT_CONFIRMED), the session timeout
// (TIMEOUT), or a refused commit.

#include <string.h>
#include "updater.h"
#include "updater_port.h"
#include "update_flash.h"
#include "update_image.h"
#include "update_build_info.h"
#include "update_gesture.h"
#include "update_led.h"
#include "update_commit.h"
#include "update_split.h"
#include "update_pair.h"   // the boot relay of a pair UF2 (docs/pair-uf2.md)
#include "build_reset.h" // BUILD_RESET_TO_* (SETTINGS_RESET modes, D44)
#include "sha512.h" // vendor/: SHA-512 from Monocypher

#ifdef SVAL_UPDATER_HOST_TEST
enum { id_custom_set_value = 0x07, id_custom_get_value = 0x08 };
#else
#    include "via.h"
#endif

// Sizes from the raw HID report (RAW_EPSIZE, keyboards/svalboard/config.h, which
// QMK and the host tests both include first).
#ifndef RAW_EPSIZE
#    error "updater: RAW_EPSIZE must be set (keyboards/svalboard/config.h)"
#endif
#define WRAPPER_BYTES 6                             // CLIENT_WRAPPER_OVERHEAD: [0xDD][client_id:4][0xFE]
#define VALUE_BYTES (RAW_EPSIZE - WRAPPER_BYTES - 3) // value_data that survives the client wrapper: 64 - 6 - 3
#define HAND_SELF 0xFF                              // INFO only
#define MANIFEST_DATA_MAX (VALUE_BYTES - 3)         // after [hand][off][n]: 52
#define CHUNK_DATA_MAX (VALUE_BYTES - 5)            // after [hand][off u24][n]: 50
#define INFO_BYTES 26                               // INFO page 0's reply, [status] through the report size
_Static_assert(VALUE_BYTES >= INFO_BYTES, "INFO page 0 fits the value bytes");
_Static_assert(MANIFEST_DATA_MAX <= UPDATE_MANIFEST_BYTES && MANIFEST_DATA_MAX <= 0xFF, "a MANIFEST piece fits its u8 count");
_Static_assert(CHUNK_DATA_MAX <= 0xFF, "a CHUNK fits its u8 count");
#define VERIFY_SLICE 2048   // image bytes hashed per updater_task() pass
#define COMMIT_DELAY_MS 100 // so the COMMIT reply reaches the host first (as scanlab.c's reboot)
#define CHORD_FLASH_MS 300  // white double flash after the chord (D4)
_Static_assert(SVAL_UPDATE_PAIR_HOLD_MS >= 90000 && SVAL_UPDATE_PAIR_HOLD_MS <= 180000, "the held approval lasts 90-180 s after DONE (D41)");

typedef struct {
    update_state_t  state;
    update_status_t last_error;
    bool            bound;  // ARM issued a nonce
    uint32_t        client; // the session's client ID; before ARM, the manifest loader's
    uint32_t        nonce;
    uint32_t        op_ms;    // last op from the session (SVAL_UPDATE_SESSION_TIMEOUT_MS)
    uint32_t        armed_ms; // ARM (SVAL_UPDATE_CONFIRM_WINDOW_MS)
    uint8_t         filled;   // bytes of manifest received
    uint32_t        image_len;
    uint32_t        erase_off, erase_end;
    uint32_t        staged; // image bytes received; whole pages of them are in the slot
    uint32_t        verify_off;
    uint32_t        crc_body;
    uint32_t        work_ms; // start of image verify
    uint16_t        verify_ms, erase_ms_max;
    uint32_t        commit_due_ms;
    bool            relay; // a session for the other half (M2)
    update_device_t dev;   // relay: the other half, as ARM checked the manifest against it
    // Protocol 4 (D41, D43).
    uint8_t pair_filled;  // bytes of the pair manifest received (a session for the other hand)
    bool    pair;         // a pair ARM: the chord approves both manifests
    bool    pre_approved; // ARM counted the chord as made (no-chord build, or the held approval)
    uint8_t pair_hash[UPDATE_SHA512_BYTES]; // sha512(pair manifest), at the pair ARM
    update_build_scan_t bi;                 // VERIFYING_IMAGE: the image's build-info records (D45)
} session_t;

// The held approval (D41): RAM only, so a reset clears it.
typedef enum { HELD_NONE = 0, HELD_PENDING, HELD_READY } held_state_t;
static struct {
    uint8_t  state;    // held_state_t: PENDING while the pair session runs, READY after its relay is DONE
    uint32_t client;   // the bound client (REBIND moves it)
    uint32_t nonce;    // the pair session's nonce
    uint32_t ready_ms; // when the relay reached DONE
    uint8_t  hash[UPDATE_SHA512_BYTES]; // sha512 of the pair manifest
} held;

static session_t         s;
static uint32_t          now_ms;
static bool              reset_due; // SETTINGS_RESET (D42): this half resets at reset_due_ms
static uint32_t          reset_due_ms;
static uint8_t           manifest[UPDATE_MANIFEST_BYTES] __attribute__((aligned(4)));
static uint8_t           pair_manifest[UPDATE_MANIFEST_BYTES] __attribute__((aligned(4))); // this half's, for one approval (D41)
static uint8_t           page[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
static crypto_sha512_ctx sha;
static uint16_t          pass_max_ms; // DIAG: the longest updater_task() pass since boot or the last clear
static uint8_t           pass_max_state;

#define MANIFEST ((const sval_update_manifest_t *)manifest)
#define PAIR_MANIFEST ((const sval_update_manifest_t *)pair_manifest)

// ---- helpers ------------------------------------------------------------------------

static uint16_t get16(const uint8_t *p) {
    return p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t get24(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t get32(const uint8_t *p) {
    return get24(p) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = (v >> 16) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
    put24(p, v);
    p[3] = (v >> 24) & 0xFF;
}
static uint16_t ms16(uint32_t ms) {
    return ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
}

// The other half answers presence and is not the same release (V).
// Also after a relay that ended DONE, until presence first reads MATCH: a
// release that changes the split table leaves presence at NONE (V).
static bool split_mismatch(void) {
    uint8_t p = updater_port_split_presence();
    return p == UPDATE_PRESENCE_VERSION || p == UPDATE_PRESENCE_SAME_HAND || p == UPDATE_PRESENCE_INVALID || update_split_awaiting_match();
}

static uint8_t self_hand(void) {
    update_device_t dev;
    update_device_self(&dev);
    return dev.hand;
}

// A relay to the other half stops with its session; once its COMMIT is out
// the relay runs on by itself to the end of the hold (update_relay_abort).
static void stop_relay(void) {
    if (s.relay) update_relay_abort();
}

static void reset_session(void) {
    stop_relay();
    update_gesture_disarm();
#ifdef SVAL_UPDATE_TEST_HOOKS
    update_commit_test_halt(UPDATE_HALT_NONE, false);
#endif
    memset(&s, 0, sizeof(s)); // IDLE, last_error OK
    memset(manifest, 0, sizeof(manifest));
    memset(pair_manifest, 0, sizeof(pair_manifest));
    memset(page, 0, sizeof(page));
}

static void held_clear(void) {
    memset(&held, 0, sizeof(held));
}

// The chord made (or counted as made) in a pair session: one approval for
// this half's install too (D41), bound to the pair manifest, the client and
// the nonce.
static void held_capture(void) {
    if (!s.pair || !s.bound || held.state != HELD_NONE || !update_gesture_done(NULL)) return;
    held.state  = HELD_PENDING;
    held.client = s.client;
    held.nonce  = s.nonce;
    memcpy(held.hash, s.pair_hash, sizeof(held.hash));
}

static update_status_t fail(update_status_t status) {
    held_clear(); // any error, in either session (D41)
    stop_relay();
    update_gesture_disarm();
    s.state      = UPDATE_STATE_ERROR;
    s.last_error = status;
    return status;
}

// The other half, for a session with its hand: its side and pointing device
// by split presence (reported, not checked: D33), this build's floors (both halves run the same release,
// V; the other half checks the manifest again itself, D7). Only a half that
// answers presence as MATCH or VERSION can be updated through this one.
static update_status_t other_device(update_device_t *dev) {
    update_presence_t p;
    uint8_t           st = update_split_other(&p);
    if (st != UPDATE_PRESENCE_MATCH && st != UPDATE_PRESENCE_VERSION) return UPDATE_UNAVAILABLE;
    update_device_self(dev);
    if (p.hand == dev->hand) return UPDATE_UNAVAILABLE;
    dev->hand        = p.hand;
    dev->pointing_id = p.pointing_id;
    return UPDATE_OK;
}

// The bound session, by client ID and (when given) nonce.
static update_status_t session(uint32_t client, const uint8_t *nonce) {
    if (!s.bound) return UPDATE_INVALID;
    if (client != s.client || (nonce && get32(nonce) != s.nonce)) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    return UPDATE_OK;
}

// Uncached slot bytes into RAM, a word at a time (off and n multiples of 4).
static bool slot_copy(uint32_t off, uint8_t *dst, uint32_t n) {
    const volatile uint32_t *p = (const volatile uint32_t *)update_slot_read(off, n);
    if (!p) return false;
    for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)dst)[i] = p[i];
    return true;
}

// ---- ops ------------------------------------------------------------------------------

// INFO page 1 (M3, D17): [status][1][fw_version u32][version 16 chars], from
// the build-info record (update_build_info.h), the same record make_update.py takes
// this image's manifest fields from.
static update_status_t op_info_version(uint8_t *rsp) {
    rsp[1] = 1;
    put32(&rsp[2], sval_update_build_info.fw_version);
    memcpy(&rsp[6], sval_update_build_info.version, UPDATE_VERSION_CHARS);
    return UPDATE_OK;
}

// INFO page 0's last bytes (protocol 3): the piece sizes this half takes, so a
// host sizes MANIFEST and CHUNK from INFO, and the report size. For the other
// half too: its image is staged in this half's slot first.
static void info_sizes(uint8_t *rsp) {
    rsp[23] = CHUNK_DATA_MAX;
    rsp[24] = MANIFEST_DATA_MAX;
    rsp[25] = RAW_EPSIZE;
}

static update_status_t op_info(const uint8_t *req, uint8_t *rsp) {
    if (req[1] == 1) return op_info_version(rsp);
    if (req[1] != 0) return UPDATE_INVALID;
    update_device_t dev;
    uint8_t         jedec[UPDATE_JEDEC_BYTES];
    update_device_self(&dev);
    update_flash_jedec(jedec);
    bool die_ok  = update_flash_available();
    bool failing = updater_port_settings_failing();
    rsp[1]       = s.state;
    rsp[2]       = UPDATE_PROTOCOL_VERSION;
    put16(&rsp[3], SVAL_UPDATE_BASE / UPDATE_FLASH_SECTOR);
    put16(&rsp[5], SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    put16(&rsp[7], SVAL_UPDATE_MAX_IMAGE / UPDATE_FLASH_SECTOR);
    memcpy(&rsp[9], jedec, UPDATE_JEDEC_BYTES);
    rsp[12] = dev.pointing_id;
    rsp[13] = dev.hand;
    // The fw_version presence sends too (SVAL_FW_VERSION in firmware), so the
    // two can never disagree.
    update_build_id_t id;
    update_split_port_build_id(&id);
    put32(&rsp[14], id.fw_version);
    rsp[18] = dev.storage_format;
    put16(&rsp[19], dev.security_epoch);
    rsp[21] = (die_ok ? 0x01 : 0) | (failing ? 0x02 : 0) | (dev.release_build ? 0x04 : 0) | (split_mismatch() ? 0x20 : 0);
#ifdef SVAL_UPDATE_TEST_HOOKS
    rsp[21] |= 0x08;
#endif
#ifdef SVAL_UPDATE_NO_CHORD
    rsp[21] |= 0x80; // D43: every ARM counts as confirmed
#endif
    rsp[22] = s.last_error;
    info_sizes(rsp);
    return die_ok && !failing ? UPDATE_OK : UPDATE_UNAVAILABLE;
}

// INFO for the other half, from it over KEYBOARD_UPDATE (see the top of this file).
static update_status_t op_info_other(uint8_t *rsp) {
    uint8_t         pl[USPLIT_INFO_PAYLOAD], state = 0;
    update_status_t st = update_split_other_info(pl, &state);
    if (st != UPDATE_OK) {
        rsp[1] = 0xFF; // no answer
        return st;
    }
    rsp[1] = state;
    rsp[2] = pl[0];
    put16(&rsp[3], SVAL_UPDATE_BASE / UPDATE_FLASH_SECTOR); // the same release on both halves (V)
    put16(&rsp[5], SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    put16(&rsp[7], SVAL_UPDATE_MAX_IMAGE / UPDATE_FLASH_SECTOR);
    rsp[12] = pl[2];
    rsp[13] = pl[1];
    memcpy(&rsp[14], &pl[3], 4);
    rsp[18] = pl[11];
    memcpy(&rsp[19], &pl[12], 2);
    rsp[21] = (pl[14] & 0x0F) | (split_mismatch() ? 0x20 : 0);
    info_sizes(rsp);
    return (pl[14] & 0x01) && !(pl[14] & 0x02) ? UPDATE_OK : UPDATE_UNAVAILABLE;
}

static update_status_t manifest_load(uint32_t client, const uint8_t *req, bool to_other) {
    uint8_t off = req[1], n = req[2];
    if (s.state != UPDATE_STATE_IDLE && s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (s.state == UPDATE_STATE_MANIFEST_LOADING && client != s.client) return UPDATE_OTHER_CLIENT;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    if (to_other) {
        update_device_t dev;
        if (other_device(&dev) != UPDATE_OK) return UPDATE_UNAVAILABLE; // the other half must answer presence
    }
    if (n == 0 || n > MANIFEST_DATA_MAX || off + n > UPDATE_MANIFEST_BYTES) return UPDATE_INVALID;
    // In order; offset 0 starts the load again.
    if (off != 0 && (s.state == UPDATE_STATE_IDLE || off != s.filled)) return UPDATE_OUT_OF_ORDER;
    if (s.state == UPDATE_STATE_IDLE) {
        // A new session: the held approval survives only this half's own
        // install from its bound client (D41).
        if (held.state != HELD_NONE && (to_other || client != held.client)) held_clear();
        reset_session();
        s.state  = UPDATE_STATE_MANIFEST_LOADING;
        s.client = client;
        s.relay  = to_other;
    }
    memcpy(manifest + off, &req[3], n);
    s.filled = off + n;
    s.op_ms  = now_ms;
    return UPDATE_OK;
}

static update_status_t op_manifest(uint32_t client, const uint8_t *req, uint8_t *rsp, bool to_other) {
    update_status_t st = manifest_load(client, req, to_other);
    rsp[1]             = s.state == UPDATE_STATE_MANIFEST_LOADING ? s.filled : 0;
    return st;
}

// PAIR_MANIFEST (D41): this half's own manifest, into its own buffer, while a
// session for the other hand loads its manifest. Refusals change nothing.
static update_status_t pair_manifest_load(uint32_t client, const uint8_t *req, bool to_other) {
    if (to_other) return UPDATE_INVALID; // [hand] is this half's own
    if (s.state == UPDATE_STATE_IDLE) return UPDATE_INVALID;
    if (s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (!s.relay) return UPDATE_INVALID; // only beside a session for the other hand
    if (client != s.client) return UPDATE_OTHER_CLIENT;
    uint8_t off = req[1], n = req[2];
    if (n == 0 || n > MANIFEST_DATA_MAX || off + n > UPDATE_MANIFEST_BYTES) return UPDATE_INVALID;
    if (off != 0 && off != s.pair_filled) return UPDATE_OUT_OF_ORDER; // in order; offset 0 starts again
    memcpy(pair_manifest + off, &req[3], n);
    s.pair_filled = off + n;
    s.op_ms       = now_ms;
    return UPDATE_OK;
}

static update_status_t op_pair_manifest(uint32_t client, const uint8_t *req, uint8_t *rsp, bool to_other) {
    update_status_t st = pair_manifest_load(client, req, to_other);
    rsp[1]             = s.state == UPDATE_STATE_MANIFEST_LOADING && s.relay ? s.pair_filled : 0;
    return st;
}

static update_status_t op_arm(uint32_t client, const uint8_t *req, uint8_t *rsp) {
    if (s.state == UPDATE_STATE_IDLE) return UPDATE_INVALID;
    if (s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (client != s.client) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    if (s.filled != UPDATE_MANIFEST_BYTES) return UPDATE_INVALID;
    // Protocol 4: the mode (a protocol 3 host sends 0). A pair ARM is for the
    // other hand, with the whole pair manifest loaded.
    uint8_t mode = req[1];
    if (mode > UPDATE_ARM_MODE_PAIR) return UPDATE_INVALID;
    if (mode == UPDATE_ARM_MODE_PAIR && (!s.relay || s.pair_filled != UPDATE_MANIFEST_BYTES)) return UPDATE_INVALID;
    if (!update_flash_available() || updater_port_settings_failing()) return UPDATE_UNAVAILABLE; // D26, D12
    // A Scan Lab sweep sets the scan timing from the host; the chord is only
    // read from normally scanned frames (and scanlab.c refuses to start a
    // sweep while the updater is active).
    if (updater_port_scan_override()) return UPDATE_BUSY;
    // The fields now, so the user is never asked to approve an image this half
    // would refuse; BEGIN checks them again before the erase.
    update_device_t dev;
    if (s.relay) {
        if (other_device(&dev) != UPDATE_OK) return UPDATE_UNAVAILABLE; // refused: nothing changes
    } else {
        update_device_self(&dev);
    }
    update_status_t st = update_manifest_check(MANIFEST, &dev);
    if (st != UPDATE_OK) return fail(st);
    if (mode == UPDATE_ARM_MODE_PAIR) {
        // The pair manifest against this half's own build and floors too (D41),
        // so the one chord never approves a file this half would refuse.
        update_device_t self;
        update_device_self(&self);
        st = update_manifest_check(PAIR_MANIFEST, &self);
        if (st != UPDATE_OK) return fail(st);
    }

    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512(hash, manifest, UPDATE_MANIFEST_BYTES); // the host shows these bytes (D5)
    uint8_t flags = 0;
#ifdef SVAL_UPDATE_NO_CHORD
    flags |= UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_NO_CHORD; // D43
#endif
    if (mode == UPDATE_ARM_MODE_PAIR) {
        s.pair = true;
        flags |= UPDATE_ARM_PAIR;
        crypto_sha512(s.pair_hash, pair_manifest, UPDATE_MANIFEST_BYTES);
    } else if (!s.relay && held.state == HELD_READY && client == held.client) {
        // This half's own install after a pair session (D41): pre-approved if
        // it is the very manifest that was approved. Used up either way.
        bool match = memcmp(hash, held.hash, UPDATE_SHA512_BYTES) == 0;
        held_clear();
        if (match) flags |= UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR;
    } else if (!s.relay) {
        held_clear(); // an ARM for this half uses the approval up
    }

    s.dev          = dev;
    s.image_len    = MANIFEST->image_len;
    s.nonce        = updater_port_random32();
    s.bound        = true;
    s.armed_ms     = now_ms;
    s.state        = UPDATE_STATE_CONFIRM_WAIT;
    s.pre_approved = (flags & UPDATE_ARM_PRE_APPROVED) != 0;
    if (flags & UPDATE_ARM_BY_PAIR) {
        update_gesture_preapprove();
    } else {
        update_gesture_arm(); // a no-chord build counts the chord as made here
    }
    held_capture(); // a pair ARM on a no-chord build: approved now
    put32(&rsp[1], s.nonce);
    memcpy(&rsp[5], hash, 4);
    if (s.pair) memcpy(&rsp[9], s.pair_hash, 4);
    rsp[13] = flags;
    return UPDATE_OK;
}

static update_status_t op_begin(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state != UPDATE_STATE_CONFIRM_WAIT) return UPDATE_INVALID;
    if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;
    if (!update_gesture_done(NULL)) return UPDATE_NOT_CONFIRMED;
    held_capture(); // a pair session: the chord approved this half's install too (D41)
    update_gesture_disarm();
    s.state = UPDATE_STATE_VERIFYING_MANIFEST;
    return UPDATE_ACCEPTED;
}

static update_status_t chunk(uint32_t client, const uint8_t *req) {
    if (!s.bound) return UPDATE_INVALID;
    if (client != s.client) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    if (s.state == UPDATE_STATE_VERIFYING_MANIFEST || s.state == UPDATE_STATE_ERASING) return UPDATE_BUSY;
    if (s.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
    uint32_t       off  = get24(&req[1]);
    uint8_t        n    = req[4];
    const uint8_t *data = &req[5];
    if (n == 0 || n > CHUNK_DATA_MAX) return UPDATE_INVALID;
    if (off < s.staged) return off + n <= s.staged ? UPDATE_OK : UPDATE_OUT_OF_ORDER; // a repeat: acknowledged, not rewritten
    if (off != s.staged) return UPDATE_OUT_OF_ORDER;
    if (n > s.image_len - s.staged) return fail(UPDATE_OVERRUN);
    while (n) {
        uint32_t at   = s.staged % UPDATE_FLASH_PAGE;
        uint32_t take = UPDATE_FLASH_PAGE - at < n ? UPDATE_FLASH_PAGE - at : n;
        memcpy(page + at, data, take);
        s.staged += take;
        data += take;
        n -= take;
        if (s.staged % UPDATE_FLASH_PAGE == 0) {
            // image_len is whole pages, so the last chunk always lands here.
            if (update_flash_program_page(s.staged - UPDATE_FLASH_PAGE, page) != UPDATE_OK) return fail(UPDATE_FLASH_ERR);
        }
    }
    return UPDATE_OK;
}

static update_status_t op_chunk(uint32_t client, const uint8_t *req, uint8_t *rsp) {
    update_status_t st = chunk(client, req);
    put24(&rsp[1], s.staged);
    return st;
}

static update_status_t op_end(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state == UPDATE_STATE_VERIFYING_MANIFEST || s.state == UPDATE_STATE_ERASING) return UPDATE_BUSY;
    if (s.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
    if (s.staged != s.image_len) return UPDATE_OUT_OF_ORDER; // END before the last byte
    crypto_sha512_init(&sha);
    update_build_scan_init(&s.bi);
    s.verify_off = 0;
    s.crc_body   = UPDATE_CRC32_INIT;
    s.work_ms    = now_ms;
    s.state      = UPDATE_STATE_VERIFYING_IMAGE;
    return UPDATE_ACCEPTED;
}

static update_status_t op_status(uint32_t client, uint8_t *rsp) {
    if (s.state != UPDATE_STATE_IDLE && client == s.client) s.op_ms = now_ms;
    rsp[1] = s.state;
    put24(&rsp[2], s.staged);
    put24(&rsp[5], s.image_len);
    put16(&rsp[8], s.erase_off / UPDATE_FLASH_SECTOR);
    put16(&rsp[10], s.erase_end / UPDATE_FLASH_SECTOR);
    rsp[12] = s.last_error;
    // rsp[13..14] reserved (0): protocol 1's signature time
    put16(&rsp[15], s.verify_ms);
    put16(&rsp[17], s.erase_ms_max);
    put16(&rsp[19], s.state == UPDATE_STATE_VERIFIED || s.state == UPDATE_STATE_COMMITTING ? s.crc_body & 0xFFFF : 0);
    rsp[21] = (update_gesture_done(NULL) && s.bound ? 0x01 : 0) | (s.bound ? 0x02 : 0) | (updater_port_settings_failing() ? 0x04 : 0);
    rsp[22] = updater_port_split_presence();
    rsp[23] = (held.state != HELD_NONE ? 0x01 : 0) | (s.bound && s.pre_approved ? 0x02 : 0); // protocol 4
    return UPDATE_OK;
}

// COMMIT in a session for the other half: the relay, then its commit.
static update_status_t relay_commit(const uint8_t *req) {
    if (s.state != UPDATE_STATE_VERIFIED && s.state != UPDATE_STATE_RELAYED) return UPDATE_INVALID;
    if (get16(&req[5]) != (s.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
    if (s.state == UPDATE_STATE_VERIFIED) {
        // Staged and verified here; now from this slot to the other half. The
        // link pauses at once; refused (still VERIFIED) if it is down.
        if (!update_relay_start(manifest, s.image_len, s.crc_body)) return UPDATE_UNAVAILABLE;
        s.state = UPDATE_STATE_RELAYING;
        return UPDATE_ACCEPTED;
    }
    if (!update_relay_commit()) return UPDATE_UNAVAILABLE;
    s.state = UPDATE_STATE_SUBSIDE_COMMITTING;
    return UPDATE_ACCEPTED;
}

static update_status_t op_commit(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.relay) return relay_commit(req);
    if (s.state != UPDATE_STATE_VERIFIED) return UPDATE_INVALID;
    if (get16(&req[5]) != (s.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
    if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;
    if (!update_commit_available()) return UPDATE_UNSUPPORTED;
    s.state         = UPDATE_STATE_COMMITTING;
    s.commit_due_ms = now_ms + COMMIT_DELAY_MS;
    return UPDATE_ACCEPTED;
}

static update_status_t op_abort(uint32_t client, const uint8_t *req) {
    switch (s.state) {
        case UPDATE_STATE_IDLE:
            held_clear(); // between the two sessions of a pair update (D41)
            return UPDATE_OK;
        case UPDATE_STATE_ERROR:
            break; // anyone
        case UPDATE_STATE_MANIFEST_LOADING:
            if (client != s.client) return UPDATE_OTHER_CLIENT;
            break;
        default: {
            update_status_t st = session(client, &req[1]);
            if (st != UPDATE_OK) return st;
            break;
        }
    }
    held_clear();
    reset_session(); // never erases: BEGIN erases anyway
    return UPDATE_OK;
}

#ifdef SVAL_UPDATE_TEST_HOOKS
static update_status_t op_test_halt(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state != UPDATE_STATE_VERIFIED || s.relay) return UPDATE_INVALID;
    return update_commit_test_halt(req[5], req[6] != 0) ? UPDATE_OK : UPDATE_INVALID;
}
#endif

// ---- reset breadcrumbs (M1 hardware debugging) -----------------------------------------------
// Watchdog scratch 0-3 survive every reset except power-on and the RUN pin
// (the boot ROM and the commit use only scratch 4-7). scratch0 marks them
// valid, scratch1 = last op << 24 | state << 16 | status << 8 | op count,
// scratch2 = ms of that op, scratch3 = ms of the last updater_task pass. At
// boot the previous values and the reset reason are latched for DIAG.
// scratch0 = CRUMB_MAGIC_COMMIT instead is the commit marker
// (updater_port_commit_mark): the reset came from this half's own commit.
#ifndef SVAL_UPDATER_HOST_TEST
#    include "hardware/structs/watchdog.h"
#    include "hardware/structs/vreg_and_chip_reset.h"
#    define CRUMB_MAGIC 0x5C0B0001u
#    define CRUMB_MAGIC_COMMIT 0x5C0B0002u
static bool     crumbs_latched;
static bool     crumbs_committed; // the last reset came from this half's own commit
static uint8_t  reset_flags; // bit0 POR/brown-out, 1 RUN pin, 2 debugger, 3 watchdog timer, 4 watchdog force, 7 crumbs valid
static uint32_t prev_crumb[3];
static uint8_t  crumb_count;

static void crumbs_latch(void) {
    if (crumbs_latched) return;
    crumbs_latched = true;
    uint32_t cr = vreg_and_chip_reset_hw->chip_reset, wr = watchdog_hw->reason;
    reset_flags = (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS ? 0x01 : 0) | (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS ? 0x02 : 0) | (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS ? 0x04 : 0) | (wr & WATCHDOG_REASON_TIMER_BITS ? 0x08 : 0) | (wr & WATCHDOG_REASON_FORCE_BITS ? 0x10 : 0);
    if (watchdog_hw->scratch[0] == CRUMB_MAGIC || watchdog_hw->scratch[0] == CRUMB_MAGIC_COMMIT) {
        crumbs_committed = watchdog_hw->scratch[0] == CRUMB_MAGIC_COMMIT;
        reset_flags |= 0x80;
        for (int i = 0; i < 3; i++) prev_crumb[i] = watchdog_hw->scratch[i + 1];
    }
    watchdog_hw->scratch[1] = watchdog_hw->scratch[2] = watchdog_hw->scratch[3] = 0;
    watchdog_hw->scratch[0] = CRUMB_MAGIC;
}

static void crumb_op(uint8_t op, uint8_t st) {
    crumbs_latch();
    watchdog_hw->scratch[1] = (uint32_t)op << 24 | (uint32_t)s.state << 16 | (uint32_t)st << 8 | crumb_count++;
    watchdog_hw->scratch[2] = updater_port_now_ms();
}

static void crumb_pass(void) {
    crumbs_latch();
    watchdog_hw->scratch[3] = updater_port_now_ms();
}

void updater_port_commit_mark(bool on) {
    crumbs_latch();
    watchdog_hw->scratch[0] = on ? CRUMB_MAGIC_COMMIT : CRUMB_MAGIC;
}

bool updater_port_commit_mark_take(void) {
    crumbs_latch();
    bool was         = crumbs_committed;
    crumbs_committed = false;
    return was;
}

static void crumbs_report(uint8_t *rsp) {
    crumbs_latch();
    rsp[8] = reset_flags;
    for (int i = 0; i < 3; i++) put32(&rsp[9 + 4 * i], prev_crumb[i]);
}
#else
#    define crumb_op(op, st) ((void)0)
#    define crumb_pass() ((void)0)
#    define crumbs_report(rsp) ((void)0)
#endif

static update_status_t op_diag(const uint8_t *req, uint8_t *rsp) {
    uint16_t size   = 0;
    uint16_t unused = updater_port_stack_free(&size);
    put16(&rsp[1], unused);
    put16(&rsp[3], size);
    put16(&rsp[5], pass_max_ms);
    rsp[7] = pass_max_state;
    crumbs_report(rsp);
    if (req[1] == 1) {
        pass_max_ms    = 0;
        pass_max_state = 0;
    }
    return UPDATE_OK;
}

static update_status_t op_relay(uint32_t client, uint8_t *rsp) {
    update_relay_info_t ri;
    update_relay_info(&ri);
    if (s.state != UPDATE_STATE_IDLE && client == s.client) s.op_ms = now_ms;
    rsp[1] = ri.phase;
    rsp[2] = ri.slave_state;
    rsp[3] = ri.slave_error;
    put24(&rsp[4], ri.acked);
    put24(&rsp[7], ri.image_len);
    put16(&rsp[10], ri.slave_erased);
    put16(&rsp[12], ri.slave_to_erase);
    put16(&rsp[14], ri.retries);
    put32(&rsp[16], ri.elapsed_ms);
    rsp[20] = (ri.paused ? 0x01 : 0) | (ri.unconfirmed ? 0x02 : 0);
    rsp[21] = ri.error;
    rsp[22] = s.state;
    return UPDATE_OK;
}

// The ops that act on a session for one hand (MANIFEST starts one).
static bool session_op(uint8_t op) {
    return op == UPDATE_OP_MANIFEST || op == UPDATE_OP_ARM || op == UPDATE_OP_BEGIN || op == UPDATE_OP_CHUNK || op == UPDATE_OP_END || op == UPDATE_OP_COMMIT || op == UPDATE_OP_TEST_HALT;
}

static update_status_t op_rebind(uint32_t client, const uint8_t *req) {
    uint32_t nonce = get32(&req[1]);
    bool     moved = false;
    if (held.state != HELD_NONE && nonce == held.nonce) {
        // The held approval follows its client in any state (D41); so does a
        // manifest that client is loading for this half's install.
        if (s.state == UPDATE_STATE_MANIFEST_LOADING && s.client == held.client) s.client = client;
        held.client = client;
        moved       = true;
    }
    if (!s.bound) return moved ? UPDATE_OK : UPDATE_INVALID;
    if (nonce != s.nonce) return moved ? UPDATE_OK : UPDATE_OTHER_CLIENT;
    s.client = client; // the old ID is no longer accepted
    s.op_ms  = now_ms;
    return UPDATE_OK;
}

// SETTINGS_RESET (D42; see the top of this file).
static update_status_t op_settings_reset(const uint8_t *req, uint8_t *rsp, bool to_other) {
    if (to_other) return UPDATE_INVALID; // [hand] is this half's own
    // req[6]: the mode (D44): 0 firmware defaults (a protocol 4 host from
    // before D44 sends 0 there), 1 a blank board.
    uint8_t mode = req[6];
    if (req[1] != 'R' || req[2] != 'S' || req[3] != 'E' || req[4] != 'T' || req[5] > 1 || mode > BUILD_RESET_MODE_MAX) return UPDATE_INVALID;
    if (s.state != UPDATE_STATE_IDLE || held.state != HELD_NONE || reset_due || update_relay_busy() || update_split_slave_active()) return UPDATE_BUSY;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE; // no build reset record on a smaller die
    if (req[5] == 1) {
        // The other half first: with no answer from it, nothing changes.
        update_presence_t p;
        uint8_t           pr = update_split_other(&p);
        if (pr != UPDATE_PRESENCE_MATCH && pr != UPDATE_PRESENCE_VERSION) return UPDATE_UNAVAILABLE;
        update_status_t st = update_split_other_settings_reset(mode);
        if (st != UPDATE_OK) return st;
        rsp[1] |= 0x02;
    }
    if (!updater_port_settings_invalidate(mode)) return UPDATE_FLASH_ERR;
    rsp[1] |= 0x01;
    reset_due    = true;
    reset_due_ms = now_ms + SVAL_UPDATE_SETTINGS_RESET_MS; // the reply goes out first
    return UPDATE_OK;
}

void updater_via_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd = data[0], op = data[2];
    uint8_t *value = &data[3];
    if (cmd != id_custom_set_value && cmd != id_custom_get_value) return; // id_custom_save: nothing here
    if (length < 3 + VALUE_BYTES) {
        if (length > 3) value[0] = UPDATE_INVALID;
        return;
    }
    uint8_t req[VALUE_BYTES], rsp[VALUE_BYTES];
    memcpy(req, value, VALUE_BYTES);
    memset(rsp, 0, VALUE_BYTES);
    now_ms = updater_port_now_ms();

    uint32_t        client  = 0;
    bool            wrapped = updater_port_client(&client);
    update_status_t st;
#ifdef SVAL_UPDATE_TEST_HOOKS
    const bool test_halt_op = false;
#else
    const bool test_halt_op = op == UPDATE_OP_TEST_HALT; // not in this build
#endif
    uint8_t me       = self_hand();
    bool    to_other = req[0] == (me ^ 1);
    if (op > UPDATE_OP_SETTINGS_RESET || test_halt_op) {
        st = UPDATE_INVALID;
    } else if (op != UPDATE_OP_INFO && !wrapped) {
        st = UPDATE_INVALID; // unwrapped VIA reaches here too (sval.c); only INFO is open
    } else if (req[0] != me && !to_other && !(op == UPDATE_OP_INFO && req[0] == HAND_SELF)) {
        st = UPDATE_UNSUPPORTED;
    } else if (to_other && (op == UPDATE_OP_TEST_HALT || op == UPDATE_OP_DIAG)) {
        st = UPDATE_UNSUPPORTED; // this half's only
    } else if (update_pair_busy() && op != UPDATE_OP_INFO && op != UPDATE_OP_STATUS && op != UPDATE_OP_RELAY && op != UPDATE_OP_DIAG) {
        // The boot relay of a pair UF2 runs (docs/pair-uf2.md): every op that
        // could start or touch a session waits for it. INFO, STATUS, RELAY
        // (its progress) and DIAG are answered.
        st = UPDATE_BUSY;
    } else if ((s.state == UPDATE_STATE_COMMITTING || s.state == UPDATE_STATE_SUBSIDE_COMMITTING) && op != UPDATE_OP_INFO && op != UPDATE_OP_STATUS && op != UPDATE_OP_RELAY &&
               !(op == UPDATE_OP_REBIND && s.state == UPDATE_STATE_SUBSIDE_COMMITTING)) {
        // Protocol 4: REBIND is answered while the other half commits, so a
        // client ID renewed then keeps the session and the held approval (D41).
        st = UPDATE_BUSY;
    } else if (s.state != UPDATE_STATE_IDLE && session_op(op) && to_other != s.relay) {
        st = UPDATE_BUSY; // a session for the other hand is running
    } else if (op == UPDATE_OP_INFO && to_other) {
        st = req[1] == 0 ? op_info_other(rsp) : UPDATE_INVALID; // M2: the other half answers; page 1 is this half's only
    } else {
        crumb_op(op, 0xFF); // 0xFF: inside the op
        switch (op) {
            case UPDATE_OP_INFO:
                st = op_info(req, rsp);
                break;
            case UPDATE_OP_MANIFEST:
                st = op_manifest(client, req, rsp, to_other);
                break;
            case UPDATE_OP_ARM:
                st = op_arm(client, req, rsp);
                break;
            case UPDATE_OP_PAIR_MANIFEST:
                st = op_pair_manifest(client, req, rsp, to_other);
                break;
            case UPDATE_OP_SETTINGS_RESET:
                st = op_settings_reset(req, rsp, to_other);
                break;
            case UPDATE_OP_BEGIN:
                st = op_begin(client, req);
                break;
            case UPDATE_OP_CHUNK:
                st = op_chunk(client, req, rsp);
                break;
            case UPDATE_OP_END:
                st = op_end(client, req);
                break;
            case UPDATE_OP_STATUS:
                st = op_status(client, rsp);
                break;
            case UPDATE_OP_COMMIT:
                st = op_commit(client, req);
                break;
            case UPDATE_OP_ABORT:
                st = op_abort(client, req);
                break;
#ifdef SVAL_UPDATE_TEST_HOOKS
            case UPDATE_OP_TEST_HALT:
                st = op_test_halt(client, req);
                break;
#endif
            case UPDATE_OP_DIAG:
                st = op_diag(req, rsp);
                break;
            case UPDATE_OP_RELAY:
                st = op_relay(client, rsp);
                break;
            default: // UPDATE_OP_REBIND
                st = op_rebind(client, req);
                break;
        }
        if (op == UPDATE_OP_BEGIN || op == UPDATE_OP_END || op == UPDATE_OP_COMMIT || op == UPDATE_OP_ABORT || op == UPDATE_OP_REBIND || op == UPDATE_OP_TEST_HALT) rsp[1] = s.state;
        crumb_op(op, st);
    }
    rsp[0] = st;
    memcpy(value, rsp, VALUE_BYTES);
}

// ---- slow work, one slice per pass ---------------------------------------------------------

static void verify_manifest(void) {
    update_device_t dev;
    if (s.relay) {
        dev = s.dev; // the other half, as ARM found it
    } else {
        update_device_self(&dev);
    }
    update_status_t st = update_manifest_check(MANIFEST, &dev);
    if (st != UPDATE_OK) {
        fail(st);
        return;
    }
    // Erase [0, image_len) rounded up to whole sectors; sectors that already
    // read erased are skipped.
    s.erase_off = 0;
    s.erase_end = (s.image_len + UPDATE_FLASH_SECTOR - 1) / UPDATE_FLASH_SECTOR * UPDATE_FLASH_SECTOR;
    s.state     = UPDATE_STATE_ERASING;
}

static void erase_step(void) {
    if (s.erase_off < s.erase_end) {
        uint32_t        t0 = updater_port_now_ms();
        update_status_t st = update_flash_erase_sector(s.erase_off);
        uint16_t        ms = ms16(updater_port_now_ms() - t0);
        if (ms > s.erase_ms_max) s.erase_ms_max = ms;
        if (st != UPDATE_OK) {
            fail(st == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
            return;
        }
        s.erase_off += UPDATE_FLASH_SECTOR;
    }
    if (s.erase_off >= s.erase_end) {
        s.staged = 0;
        s.state  = UPDATE_STATE_RECEIVING;
    }
}

static void verify_image_step(void) {
    uint8_t  buf[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
    uint32_t end = s.image_len - s.verify_off > VERIFY_SLICE ? s.verify_off + VERIFY_SLICE : s.image_len;
    while (s.verify_off < end) {
        uint32_t n = end - s.verify_off < sizeof(buf) ? end - s.verify_off : sizeof(buf);
        if (!slot_copy(s.verify_off, buf, n)) {
            fail(UPDATE_FLASH_ERR);
            return;
        }
        crypto_sha512_update(&sha, buf, n);
        update_build_scan(&s.bi, buf, n);
        uint32_t skip = s.verify_off < UPDATE_VECTORS_OFFSET ? UPDATE_VECTORS_OFFSET - s.verify_off : 0;
        if (skip < n) s.crc_body = update_crc32_mpeg2_update(s.crc_body, buf + skip, n - skip);
        s.verify_off += n;
    }
    if (s.verify_off < s.image_len) return;

    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512_final(&sha, hash);
    update_status_t st = memcmp(hash, MANIFEST->sha512, UPDATE_SHA512_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_HASH;
    if (st == UPDATE_OK) {
        uint8_t head[UPDATE_HEAD_BYTES] __attribute__((aligned(4)));
        st = slot_copy(0, head, sizeof(head)) ? update_image_head_check(head, s.image_len) : UPDATE_FLASH_ERR;
    }
    // D45: what the image itself says it is. A release build never takes a
    // no-chord or kept-settings dev image, even from a manifest without
    // DIAGNOSTIC. s.dev is the half the image is for (this build's floors and
    // release state either way); the other half checks again itself (D7).
    if (st == UPDATE_OK) st = update_image_build_check(&s.bi, &s.dev);
    s.verify_ms = ms16(updater_port_now_ms() - s.work_ms);
    if (st != UPDATE_OK) {
        fail(st);
        return;
    }
    s.state = UPDATE_STATE_VERIFIED;
}

// ---- the lights (D24, update_led.h) ----------------------------------------------------
//
// One meaning per color, and red only for a failure:
//   blue blink       your turn: make the chord (the half with USB)
//   white flash      got it: the chord was recognized
//   rainbow          updating, keep plugged in: on both halves, from the start
//                    of an update until the halves match, including while one
//                    half waits for the other to restart after a relay
//   magenta          this half's own commit, which holds a fixed color while
//                    it writes from RAM (update_led_commit_latch)
//   green flash      done: presence matches after an update or a relay
//   red, 1 Hz        it failed, open Keybard: a refused or failed install, a
//                    refused, damaged or tried-too-often pair record, or halves
//                    that still differ SVAL_UPDATE_AFTER_WAIT_MS after an update.
//                    Every red stops after SVAL_UPDATE_RED_MS; INFO flags bit5
//                    and Keybard keep reporting the problem.
//   none             nothing for you to do (a pair record kept waiting for an
//                    absent other half shows nothing)
//
// The after-update window. The half with USB opens it when a relay ends DONE
// (update_split_awaiting_match() rises) and on the first boot after its own
// commit (the commit marker, updater_port_commit_mark); it shows the rainbow
// until presence reads MATCH (then the green flash), or until it has been idle
// SVAL_UPDATE_AFTER_WAIT_MS (then red, then nothing). The half without USB
// cannot see presence: the master's light rides on its 500 ms presence ping
// (updater_split_light, presence_rpc_t.update_light in kb/svalboard.h), and
// that half follows it: rainbow, the green flash once, red. It opens its own
// window at the first boot after its commit too, so it stays rainbow while it
// waits for the first ping. A ping without the byte (a master from before it)
// closes that window quietly: that half cannot tell, so it shows nothing.

// SVAL_UPDATE_AFTER_WAIT_MS and SVAL_UPDATE_RED_MS: kb/config.h.
#define DONE_FLASH_MS 600  // the green double flash (update_led.c's DONE_STEP_MS x 4)
#define LIGHT_DONE_MS 2000 // DONE stays on the ping this long, so the other half sees it
#define LIGHT_NO_BYTE 0xFF // a ping from a master without the byte

typedef enum {
    AFTER_NONE = 0,
    AFTER_WAIT, // rainbow: an update finished, the halves do not match yet
    AFTER_DONE, // the green flash
    AFTER_RED,  // the halves still differ: red for SVAL_UPDATE_RED_MS
} after_phase_t;

static struct {
    bool              booted;   // the first pass took the commit marker
    uint8_t           phase;    // after_phase_t
    bool              relay;    // the other half was there and is expected back (a relay ended DONE)
    bool              answered; // presence answered since the window opened
    bool              awaiting_was;
    uint32_t          ms; // WAIT: the last activity; DONE, RED: the start
    bool              done_any;
    uint32_t          done_ms;  // the last green flash (the ping carries DONE for LIGHT_DONE_MS)
    bool              red_on;   // a failure light's cause is present
    uint32_t          red_ms;   // since then
    update_led_mode_t shown;    // the last light, for the ping
    uint8_t           hint_was; // the half without USB: the last light the master sent
    uint8_t           rx_seen;  // pings handled
} lt;

// The half without USB: written by the presence ping's callback (the split
// SlaveThread), read by housekeeping. A torn read costs one pass of light.
static volatile uint8_t rx_light = LIGHT_NO_BYTE, rx_n;

void updater_split_light_seen(bool has_light, uint8_t light) {
    rx_light = has_light ? light : LIGHT_NO_BYTE;
    rx_n     = rx_n + 1;
}

uint8_t updater_split_light(void) {
    uint32_t t = updater_port_now_ms();
    if (lt.done_any && t - lt.done_ms < LIGHT_DONE_MS) return UPDATE_LIGHT_DONE;
    switch (lt.shown) {
        case UPDATE_LED_PROGRESS:
        case UPDATE_LED_WRITING:
            return UPDATE_LIGHT_UPDATING;
        case UPDATE_LED_ERROR:
            return UPDATE_LIGHT_FAILED;
        default:
            return UPDATE_LIGHT_NONE;
    }
}

static void after_open(bool relay) {
    lt.phase    = AFTER_WAIT;
    lt.relay    = relay;
    lt.answered = false;
    lt.ms       = now_ms;
}

static void after_done(void) {
    lt.phase    = AFTER_DONE;
    lt.ms       = now_ms;
    lt.done_any = true;
    lt.done_ms  = now_ms;
}

// The boot relay of a pair UF2: rainbow once the other half answers and can
// take it (a record kept waiting for an absent half shows nothing), red after a record that was
// refused, damaged or tried too often (update_pair.c, for
// SVAL_UPDATE_PAIR_ERROR_MS).
static bool pair_light(update_led_mode_t *mode, bool *failed) {
    switch (update_pair_phase()) {
        case PAIR_BOOT_HASH:
        case PAIR_BOOT_WAIT:
            *mode = update_pair_peer_takes() ? UPDATE_LED_PROGRESS : UPDATE_LED_NONE;
            return true;
        case PAIR_BOOT_RELAY:
        case PAIR_BOOT_RETRY:
            *mode = UPDATE_LED_PROGRESS; // the other half writes: this half holds no fixed color
            return true;
        case PAIR_BOOT_ERROR:
            *mode   = UPDATE_LED_ERROR;
            *failed = true;
            return true;
        default:
            return false;
    }
}

// This half's own update work: its light, or NONE when it has none. *failed:
// a failure light (red), whose cause may outlast the light.
static update_led_mode_t own_light(bool *failed) {
    uint32_t          done_ms;
    update_led_mode_t pair_mode;
    *failed = false;
    switch (s.state) {
        case UPDATE_STATE_IDLE:
            if (pair_light(&pair_mode, failed)) return pair_mode;
            // The half without USB, in a session from the other half.
            switch (update_split_slave_state()) {
                case UPDATE_STATE_IDLE:
                    return UPDATE_LED_NONE;
                case UPDATE_STATE_ERROR:
                    *failed = true;
                    return UPDATE_LED_ERROR;
                case UPDATE_STATE_COMMITTING:
                    return UPDATE_LED_WRITING;
                default:
                    return UPDATE_LED_PROGRESS;
            }
        case UPDATE_STATE_MANIFEST_LOADING:
            return UPDATE_LED_NONE; // nothing shown to the user yet
        case UPDATE_STATE_ERROR:
            *failed = true;
            return UPDATE_LED_ERROR;
        case UPDATE_STATE_COMMITTING:
            return UPDATE_LED_WRITING;
        case UPDATE_STATE_SUBSIDE_COMMITTING: // the other half writes
        case UPDATE_STATE_RELAYING:
        case UPDATE_STATE_RELAYED:
            return UPDATE_LED_PROGRESS;
        default:
            break;
    }
    // Pre-approved (D41, D43): no blue and no white flash, straight to the rainbow.
    if (s.pre_approved) return UPDATE_LED_PROGRESS;
    if (update_gesture_done(&done_ms)) {
        if (now_ms - done_ms < CHORD_FLASH_MS) return UPDATE_LED_CHORD;
    } else if (s.state == UPDATE_STATE_CONFIRM_WAIT) {
        return UPDATE_LED_CONFIRM;
    }
    return UPDATE_LED_PROGRESS; // approved: updating from here on
}

// The window on the half with USB.
static void after_master(bool busy) {
    uint8_t p        = updater_port_split_presence();
    bool    awaiting = update_split_awaiting_match();
    if (awaiting && !lt.awaiting_was) after_open(true); // a relay ended DONE
    lt.awaiting_was = awaiting;
    if (lt.phase != AFTER_WAIT && lt.phase != AFTER_RED) return;
    if (p != UPDATE_PRESENCE_NONE) lt.answered = true;
    if (p == UPDATE_PRESENCE_MATCH && !awaiting) {
        after_done();
        return;
    }
    if (busy && lt.phase == AFTER_WAIT) lt.ms = now_ms;
    if (lt.phase == AFTER_WAIT && now_ms - lt.ms >= SVAL_UPDATE_AFTER_WAIT_MS) {
        // Red if the other half answered and differs, or a relay sent it
        // away and it never came back; a half on its own shows nothing.
        if (lt.relay || lt.answered) {
            lt.phase = AFTER_RED;
            lt.ms    = now_ms;
        } else {
            lt.phase = AFTER_NONE;
        }
    }
}

// The window on the half without USB: the master's light from its pings.
static void after_slave(bool busy) {
    if (lt.rx_seen != rx_n) {
        lt.rx_seen    = rx_n;
        uint8_t light = rx_light;
        switch (light) {
            case UPDATE_LIGHT_UPDATING:
                if (lt.phase != AFTER_WAIT) after_open(true);
                lt.ms = now_ms;
                break;
            case UPDATE_LIGHT_DONE:
                if (lt.hint_was != UPDATE_LIGHT_DONE) after_done();
                break;
            case UPDATE_LIGHT_FAILED:
                if (lt.hint_was != UPDATE_LIGHT_FAILED) {
                    lt.phase = AFTER_RED;
                    lt.ms    = now_ms;
                }
                break;
            default: // NONE, or a master that sends no light: nothing to show
                if (lt.phase != AFTER_DONE) lt.phase = AFTER_NONE;
                break;
        }
        lt.hint_was = light;
    }
    if (busy && lt.phase == AFTER_WAIT) lt.ms = now_ms;
    // No ping for a whole wait after an update: the link never came back.
    if (lt.phase == AFTER_WAIT && now_ms - lt.ms >= SVAL_UPDATE_AFTER_WAIT_MS) {
        lt.phase = AFTER_RED;
        lt.ms    = now_ms;
    }
}

static update_led_mode_t led_mode(void) {
    bool              failed;
    update_led_mode_t own  = own_light(&failed);
    bool              busy = own != UPDATE_LED_NONE || s.state != UPDATE_STATE_IDLE || update_relay_busy() || update_pair_busy() || update_split_slave_active();
    if (!lt.booted) {
        lt.booted = true;
        if (updater_port_commit_mark_take()) after_open(false); // this half's own install just finished
    }
    if (updater_port_is_master()) {
        after_master(busy);
    } else {
        after_slave(busy);
    }
    if (failed) {
        // A failure is the last word: the window closes, and the red stops
        // after SVAL_UPDATE_RED_MS while its cause stays.
        if (lt.phase == AFTER_WAIT || lt.phase == AFTER_RED) lt.phase = AFTER_NONE;
        if (!lt.red_on) {
            lt.red_on = true;
            lt.red_ms = now_ms;
        }
        return now_ms - lt.red_ms < SVAL_UPDATE_RED_MS ? UPDATE_LED_ERROR : UPDATE_LED_NONE;
    }
    lt.red_on = false;
    if (own != UPDATE_LED_NONE) return own;
    switch (lt.phase) {
        case AFTER_WAIT:
            return UPDATE_LED_PROGRESS;
        case AFTER_DONE:
            if (now_ms - lt.ms < DONE_FLASH_MS) return UPDATE_LED_DONE;
            lt.phase = AFTER_NONE;
            return UPDATE_LED_NONE;
        case AFTER_RED:
            if (now_ms - lt.ms < SVAL_UPDATE_RED_MS) return UPDATE_LED_ERROR;
            lt.phase = AFTER_NONE;
            return UPDATE_LED_NONE;
        default:
            return UPDATE_LED_NONE;
    }
}

void updater_task(void) {
    now_ms                 = updater_port_now_ms();
    uint32_t       pass_t0 = now_ms;
    update_state_t pass_st = s.state;
    // The relay (M2) runs before the session looks at it; once its COMMIT is
    // out it runs to the end of its hold even if the session went away.
    update_relay_task();
    // The boot relay of a pair UF2 (docs/pair-uf2.md), after the relay it drives.
    update_pair_task();
    now_ms         = updater_port_now_ms();
    bool hostbound = (s.state >= UPDATE_STATE_MANIFEST_LOADING && s.state <= UPDATE_STATE_VERIFIED) || s.state == UPDATE_STATE_RELAYING || s.state == UPDATE_STATE_RELAYED;
    if (hostbound && now_ms - s.op_ms >= SVAL_UPDATE_SESSION_TIMEOUT_MS) {
        // A host that went away before the other half's COMMIT stops the relay (D23).
        // A half-sent manifest was never shown to the user: just drop it.
        if (s.state == UPDATE_STATE_MANIFEST_LOADING) {
            held_clear();
            reset_session();
        } else {
            fail(UPDATE_TIMEOUT);
        }
    }
    if (s.state == UPDATE_STATE_CONFIRM_WAIT && !update_gesture_done(NULL) && now_ms - s.armed_ms >= SVAL_UPDATE_CONFIRM_WINDOW_MS) {
        fail(UPDATE_NOT_CONFIRMED);
    }
    if (s.state == UPDATE_STATE_CONFIRM_WAIT) held_capture(); // the chord in a pair session (D41)
    if (held.state == HELD_READY && now_ms - held.ready_ms >= SVAL_UPDATE_PAIR_HOLD_MS) held_clear();
    if (reset_due && (int32_t)(now_ms - reset_due_ms) >= 0) {
        reset_due = false;
        updater_port_reboot(); // SETTINGS_RESET (D42): the next boot resets every stored setting
    }

    switch (s.state) {
        case UPDATE_STATE_VERIFYING_MANIFEST:
            verify_manifest();
            break;
        case UPDATE_STATE_ERASING:
            erase_step();
            break;
        case UPDATE_STATE_VERIFYING_IMAGE:
            verify_image_step();
            break;
        case UPDATE_STATE_RELAYING:
        case UPDATE_STATE_RELAYED:
        case UPDATE_STATE_SUBSIDE_COMMITTING: {
            update_relay_info_t ri;
            update_relay_info(&ri);
            if (ri.phase == RELAY_FAILED) {
                fail(ri.error);
            } else if (s.state == UPDATE_STATE_RELAYING && ri.phase == RELAY_VERIFIED) {
                s.state = UPDATE_STATE_RELAYED;
            } else if (s.state == UPDATE_STATE_SUBSIDE_COMMITTING && ri.phase == RELAY_DONE) {
                // The held approval of a pair session now waits for this half's
                // install, for SVAL_UPDATE_PAIR_HOLD_MS (D41).
                if (held.state == HELD_PENDING && held.nonce == s.nonce) {
                    held.state    = HELD_READY;
                    held.ready_ms = now_ms;
                } else {
                    held_clear();
                }
                reset_session(); // the other half took COMMIT and reset (or never answered again: V)
            } else if (ri.phase == RELAY_IDLE || ri.phase == RELAY_DONE) {
                fail(UPDATE_INVALID); // cannot happen: the relay ended outside its session
            }
            break;
        }
        case UPDATE_STATE_COMMITTING:
            if (s.relay) {
                fail(UPDATE_INVALID); // cannot happen: never this half's commit for the other hand's image
                break;
            }
            if ((int32_t)(now_ms - s.commit_due_ms) >= 0) {
                // Image bytes in RAM are cleared before the reset (commit step
                // 7, R23); these two are not needed any more.
                memset(page, 0, sizeof(page));
                memset(&sha, 0, sizeof(sha));
                // Returns only when the commit refused before touching the firmware.
                updater_port_commit_mark(true); // the next boot shows the rainbow until the halves match
                update_status_t st = update_commit_run(MANIFEST, s.crc_body);
                updater_port_commit_mark(false);
                fail(st != UPDATE_OK ? st : UPDATE_FLASH_ERR);
            }
            break;
        default:
            break;
    }
    now_ms = updater_port_now_ms();
    // The 50 ms rule (R13) on the device: the host tests only see flash time.
    if (now_ms - pass_t0 > pass_max_ms) {
        pass_max_ms    = ms16(now_ms - pass_t0);
        pass_max_state = pass_st;
    }
    lt.shown = led_mode();
    update_led_show(lt.shown);
    crumb_pass();
}

bool updater_active(void) {
    return s.state != UPDATE_STATE_IDLE || update_split_slave_active() || update_relay_busy() || update_pair_busy();
}

update_state_t updater_state(void) {
    return s.state;
}

#ifdef SVAL_UPDATER_HOST_TEST
void updater_host_reset(void) {
    update_pair_host_reset();
    memset(&lt, 0, sizeof(lt));
    rx_light = LIGHT_NO_BYTE;
    rx_n     = 0;
    reset_session();
    held_clear();
    reset_due = false;
}
#endif

// ---- firmware port (updater_port.h) ---------------------------------------------------------

#ifndef SVAL_UPDATER_HOST_TEST
#    include "timer.h"
#    include "wait.h"
#    include "client_wrapper.h"
_Static_assert(WRAPPER_BYTES == CLIENT_WRAPPER_OVERHEAD, "the client wrapper's header");
#    include "scanlab.h"
#    include "build_reset.h"
#    include "quantum.h" // soft_reset_keyboard
#    include "hardware/structs/rosc.h"
#    ifdef EEPROM_WEAR_LEVELING
#        include "wear_leveling.h"
#    else
#        error "updater: settings must be on QMK wear leveling, whose write-failure latch gates ARM and COMMIT (D12)"
#    endif

uint32_t updater_port_now_ms(void) {
    return timer_read32();
}

bool updater_port_client(uint32_t *client_id) {
    if (!client_wrapper_in_via()) return false;
    *client_id = client_wrapper_current_id();
    return *client_id != CLIENT_ID_BOOTSTRAP;
}

bool updater_port_settings_failing(void) {
    return wear_leveling_write_failed();
}

// The ring oscillator, a bit per read as identity.c does, mixed with the timer.
uint32_t updater_port_random32(void) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < 32; i++) {
        v = (v << 1) | (rosc_hw->randombit & 1);
        wait_us(1);
    }
    v ^= timer_read32() * 2654435761u;
    return v ? v : 1;
}

bool updater_port_scan_override(void) {
    return scanlab_active();
}

uint8_t updater_port_split_presence(void) {
    return update_split_presence();
}

bool updater_port_settings_invalidate(uint8_t mode) {
    return build_reset_invalidate(mode);
}

void updater_port_reboot(void) {
    soft_reset_keyboard();
}

bool updater_port_is_master(void) {
    return is_keyboard_master();
}

// crt0 (ChibiOS crt0_v6m.S, CRT0_INIT_STACKS on by default) fills the process
// stack with this before main(); main() runs on that stack. The count is a
// little optimistic if a used word happens to hold the pattern.
#    define STACK_FILL 0x55555555u
extern uint32_t __process_stack_base__[], __process_stack_end__[];

uint16_t updater_port_stack_free(uint16_t *size) {
    const volatile uint32_t *p = __process_stack_base__;
    while (p < __process_stack_end__ && *p == STACK_FILL) p++;
    *size = (uint16_t)((uintptr_t)__process_stack_end__ - (uintptr_t)__process_stack_base__);
    return (uint16_t)((uintptr_t)p - (uintptr_t)__process_stack_base__);
}
#endif
