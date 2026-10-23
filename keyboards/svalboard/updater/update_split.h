// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// In-firmware updater, M2: the split link (plan docs/updater-plan.md, M2).
// Presence with version (D17, V), the KEYBOARD_UPDATE split RPC (wire format
// in update_split_wire.h), the other half's session (store and forward, D14),
// and the relay that feeds it from the half with USB.
//
// The half without USB (the slave) runs a session of its own:
//
//   IDLE -BEGIN fragments-> MANIFEST_LOADING -last fragment-> VERIFYING_MANIFEST
//   -> ERASING -> RECEIVING -END-> VERIFYING_IMAGE -> VERIFIED -COMMIT-> COMMITTING
//   (and ERROR, latched until ABORT or the 5 s timeout)
//
// The KEYBOARD_UPDATE callback runs in the SlaveThread, holding the split
// shared-memory lock (platforms/chibios/drivers/serial_protocol.c). It answers
// at once, programs one 256 B page when a page's last fragment arrives (R16),
// and never does slow work. The slow work (the manifest check, D7; the erase; the image SHA-512; the commit) runs in housekeeping
// (update_split_slave_task), behind the master's pause (D15). The two meet in
// a single-slot mailbox with a sequence number: the callback posts a job by
// changing the state, and bumps the sequence number on every new session and
// every cancel; housekeeping takes the lock to copy the job out and again to
// post its result, which it drops if the sequence number moved meanwhile
// (an ABORT or a new session won the race).
//
// The half with USB (the master) relays an image it has already staged and
// verified in its own slot through the M1 path (updater.c), from flash, a
// slice per main-loop pass (update_relay_*).

#include <stdbool.h>
#include <stdint.h>
#include "update_split_wire.h"

// ---- presence (D17) ----------------------------------------------------------------

// The half without USB, inside the KEYBOARD_SYNC_A RPC callback (SlaveThread):
// fill the response's update block (presence_rsp_t.update, after the pointer
// report) with this build's presence. Writes nothing if out is shorter than
// UPDATE_PRESENCE_BYTES.
void update_split_presence_fill(void *out, uint8_t out_len);

// The half with USB, after each presence ping: answered is the RPC's result,
// rsp its response bytes.
void update_split_presence_result(bool answered, const void *rsp, uint8_t len);

// update_presence_status_t: NONE until the other half answers, and again once
// a ping goes unanswered.
uint8_t update_split_presence(void);

// The other half answers and is not a match (VERSION, SAME_HAND or INVALID):
// INFO flags bit5 (V). The lights show it only after an update (updater.c).
bool update_split_mismatch(void);

// The other half's last presence answer (*p) and its status. Only MATCH and
// VERSION describe a half that can be updated through this one.
uint8_t update_split_other(update_presence_t *p);

// ---- KEYBOARD_UPDATE, the half without USB ---------------------------------------------

// The RPC callback body: one request frame in, one response frame out
// (out_len bytes, zero-filled past the frame). Runs in the SlaveThread (1 KiB
// stack) with the shared-memory lock held, while the master waits. The only
// flash operation it makes is one page program, when a PAGE completes a page.
void update_split_slave_rpc(const uint8_t *in, uint8_t in_len, uint8_t *out, uint8_t out_len);

// Housekeeping (from updater_task, every pass, on either half; nothing to do
// unless the slave session is active): the timeout and one slice of slow work.
void update_split_slave_task(void);

// update_state_t of the slave's session.
uint8_t update_split_slave_state(void);
bool    update_split_slave_active(void); // not IDLE: full clock, the updater's LEDs

// ---- the relay, the half with USB --------------------------------------------------------

typedef enum {
    RELAY_IDLE = 0,
    RELAY_START,       // pause the link, clear any old session on the other half
    RELAY_MANIFEST,    // BEGIN: the manifest, in fragments
    RELAY_ERASE_WAIT,  // the other half checks the manifest and erases (paused)
    RELAY_PAGES,       // PAGE: the image (the link runs: both halves type)
    RELAY_END,         // END (paused)
    RELAY_VERIFY_WAIT, // the other half verifies the image (paused)
    RELAY_VERIFIED,    // the other half holds the verified image; kept alive until COMMIT
    RELAY_COMMIT,      // COMMIT (paused)
    RELAY_HOLD,        // paused while it commits and reboots (R18)
    RELAY_PROBE,       // one STATUS after the hold: an answer from a half that did not reset is a failure
    RELAY_DONE,
    RELAY_FAILED,
    RELAY_ABORTING, // stopping: ABORT until the other half answers (or the link timeout), the link still paused
} relay_phase_t;

// Starts relaying the image staged and verified in this half's slot.
// manifest (UPDATE_MANIFEST_BYTES) is copied. The link is paused at once;
// false (and nothing started) if it cannot be (the link is down).
bool update_relay_start(const uint8_t *manifest, uint32_t image_len, uint32_t crc_body);

// The boot relay of a pair UF2 (update_pair.c, and nothing else): the same
// relay, of this half's own running image (flash offset 0), which the caller
// has hashed against manifest. No host session and no chord are involved:
// only a record that a UF2 copy in BOOTSEL wrote leads here.
bool update_relay_start_self(const uint8_t *manifest, uint32_t image_len, uint32_t crc_body);

// One pass: at most SVAL_UPDATE_RELAY_PASS_MS of relay work, or one poll.
void update_relay_task(void);

// In RELAY_VERIFIED: pause and send COMMIT. False (nothing sent) otherwise, or
// if the link cannot be paused.
bool update_relay_commit(void);

// Stops a relay before its COMMIT: RELAY_ABORTING, which sends ABORT once per
// pass until the other half answers (or for the link timeout; it times out by
// itself if they are all lost), with the link still paused if it was (the
// other half may be erasing), then RELAY_IDLE and the link resumed. A relay
// that fails ends the same way, in RELAY_FAILED. Once COMMIT was sent it does
// nothing: the other half may be writing, and the relay ends by itself after
// the hold. A finished relay (DONE, FAILED) keeps its record.
void update_relay_abort(void);

// A relay ended DONE and presence has not read MATCH since: INFO flags bit5,
// as for a version mismatch (V), and the rainbow until MATCH (updater.c).
bool update_split_awaiting_match(void);

typedef struct {
    uint8_t         phase;        // relay_phase_t
    update_status_t error;        // why it failed (RELAY_FAILED)
    uint8_t         slave_state;  // the other half's state and last error, from its last answer
    uint8_t         slave_error;
    uint32_t        acked;        // image bytes the other half has acknowledged
    uint32_t        image_len;
    uint16_t        slave_erased; // the other half's erase progress, in sectors
    uint16_t        slave_to_erase;
    uint16_t        retries;      // requests sent again for want of a valid answer
    uint32_t        elapsed_ms;   // since the start (frozen once it ends)
    bool            paused;       // the link is paused by the relay now
    bool            unconfirmed;  // DONE, but the other half never answered after COMMIT
} update_relay_info_t;

void update_relay_info(update_relay_info_t *info);
bool update_relay_busy(void); // between START and DONE/FAILED (ABORTING is busy)

// The other half's INFO through KEYBOARD_UPDATE (at most two tries; refused
// while a relay runs). UPDATE_OK with its 15 payload bytes and state, else
// UPDATE_UNAVAILABLE (no answer) or UPDATE_BUSY.
update_status_t update_split_other_info(uint8_t payload[USPLIT_INFO_PAYLOAD], uint8_t *state);

// SETTINGS_RESET (D42) on the other half, through KEYBOARD_UPDATE: it
// invalidates its build reset record and resets itself
// SVAL_UPDATE_SETTINGS_RESET_SLAVE_MS later. UPDATE_OK once it said so;
// UPDATE_BUSY while a relay runs here or a session runs there; else
// UPDATE_UNAVAILABLE (no valid answer: unplugged, or a protocol 2-3 half,
// which does not know the op; or a protocol 4 half from before D44 asked
// for mode 1, which it does not know) or its refusal (FLASH_ERR). mode 0
// sends protocol 4's 4-byte payload, mode 1 (blank, D44) one byte more.
update_status_t update_split_other_settings_reset(uint8_t mode);

// The half without USB took a SETTINGS_RESET and resets soon.
bool update_split_slave_reset_pending(void);

#ifdef SVAL_UPDATER_HOST_TEST
void update_split_host_reset(void);
void update_split_host_master_reset(void); // this half's RAM after its reset: relay, presence, latch
#else
// keyboard_post_init (main thread): probes the flash die once, so the
// SlaveThread never does, and registers the KEYBOARD_UPDATE callback.
void update_split_init(void);
#endif

// ---- port (firmware at the end of update_split.c; the host tests supply their own) ----

void     update_split_port_build_id(update_build_id_t *id);
uint8_t  update_split_port_info_flags(void); // INFO flags byte, see update_split_wire.h
bool     update_split_port_updating(void);   // updater_active()
uint32_t update_split_port_now_ms(void);
// The split shared-memory lock: the slave's housekeeping takes it around its
// mailbox copies (the callback already holds it).
void update_split_port_lock(void);
void update_split_port_unlock(void);
// Slave, in the SlaveThread: update_flash_program_page(), unless a DMA channel
// is busy (UPDATE_BUSY, nothing written), checked with interrupts off in the
// same section as the program (R6: the PMW SPI's DMA reads its TX source
// from XIP, and this thread can preempt the main thread mid-transfer).
update_status_t update_split_port_program_page(uint32_t off, const uint8_t *page);
// Slave, in the SlaveThread: invalidate this half's build reset record (D42;
// one page program, no erase), under the same DMA check: UPDATE_OK,
// UPDATE_BUSY (a DMA channel is busy: nothing written) or UPDATE_FLASH_ERR.
update_status_t update_split_port_settings_invalidate(uint8_t mode);
// Slave, from housekeeping: reset this half (SETTINGS_RESET). Never returns
// on the device.
void update_split_port_reboot(void);
// Master: one KEYBOARD_UPDATE RPC (transaction_rpc_exec); the split pause (D15).
bool update_split_port_rpc(const uint8_t *req, uint8_t req_len, uint8_t *rsp, uint8_t rsp_len);
bool update_split_port_pause(bool on);
// Master: n bytes of this half's slot at off (whole words), read uncached.
bool update_split_port_slot_read(uint32_t off, uint8_t *dst, uint32_t n);
// Master: n bytes of this half's own image (flash offset off, whole words, inside
// [0, SVAL_UPDATE_MAX_IMAGE)), read uncached. The boot relay's source.
bool update_split_port_self_read(uint32_t off, uint8_t *dst, uint32_t n);
