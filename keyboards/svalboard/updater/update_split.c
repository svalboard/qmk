// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// In-firmware updater, M2: presence, the other half's session behind the
// KEYBOARD_UPDATE split RPC, and the relay from the half with USB (see
// update_split.h and update_split_wire.h).

#include <string.h>
#include "update_split.h"
#include "update_flash.h"
#include "update_image.h"
#include "update_commit.h"
#include "updater_port.h"
#include "sha512.h" // vendor/: SHA-512 from Monocypher

static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = (v >> 16) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get24(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static bool due(uint32_t now, uint32_t at) {
    return (int32_t)(now - at) >= 0;
}

// Uncached slot bytes into RAM, a word at a time (off and n multiples of 4).
static bool slot_copy(uint32_t off, uint8_t *dst, uint32_t n) {
    if (off % 4 || n % 4) return false;
    const volatile uint32_t *p = (const volatile uint32_t *)update_slot_read(off, n);
    if (!p) return false;
    for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)dst)[i] = p[i];
    return true;
}

// ---- presence (D17) ------------------------------------------------------------------

static uint8_t           presence; // update_presence_status_t, master only
static update_presence_t presence_last;
static bool              awaiting_match; // see update_split_awaiting_match()

void update_split_presence_fill(void *out, uint8_t out_len) {
    if (out_len < UPDATE_PRESENCE_BYTES) return;
    update_build_id_t id;
    update_presence_t p;
    update_split_port_build_id(&id);
    usplit_presence_encode(&p, &id, update_split_port_updating());
    memcpy(out, &p, sizeof(p));
}

void update_split_presence_result(bool answered, const void *rsp, uint8_t len) {
    if (!answered) {
        presence = UPDATE_PRESENCE_NONE;
        return;
    }
    update_build_id_t id;
    update_split_port_build_id(&id);
    presence = usplit_presence_compare(rsp, len, &id, &presence_last);
    if (presence == UPDATE_PRESENCE_MATCH) awaiting_match = false;
}

uint8_t update_split_presence(void) {
    return presence;
}

bool update_split_mismatch(void) {
    return presence == UPDATE_PRESENCE_VERSION || presence == UPDATE_PRESENCE_SAME_HAND || presence == UPDATE_PRESENCE_INVALID || awaiting_match;
}

uint8_t update_split_other(update_presence_t *p) {
    *p = presence_last;
    return presence;
}

// ---- KEYBOARD_UPDATE, the half without USB --------------------------------------------
//
// sl is shared by the RPC callback (SlaveThread, which holds the split
// shared-memory lock for the whole transaction) and housekeeping (which takes
// the lock to read or change it). hk is housekeeping's own.

#define SLAVE_COMMIT_DELAY_MS 100 // so the COMMIT answer reaches the master first (as updater.c)
// SETTINGS_RESET (D42): this half resets this long after taking it, so its
// answer and the master's own reply get out first; both halves are reset
// within 500 ms of the host's reply.
#ifndef SVAL_UPDATE_SETTINGS_RESET_SLAVE_MS
#    define SVAL_UPDATE_SETTINGS_RESET_SLAVE_MS 300
#endif
_Static_assert(SVAL_UPDATE_SETTINGS_RESET_SLAVE_MS < 500, "both halves reset within 500 ms (D42)");
#define SLAVE_VERIFY_SLICE 2048   // image bytes hashed per housekeeping pass (as updater.c)

static struct {
    // The last answer, for a retry of the same request (same seq and CRC).
    bool     cached;
    uint8_t  seq;
    uint16_t crc;
    uint8_t  rsp[USPLIT_MSG_MAX];
    uint8_t  rsp_len;
    // The session.
    uint8_t        state;      // update_state_t
    uint8_t        last_error; // update_status_t
    uint32_t       mseq;       // mailbox sequence number: moves with every new session and every cancel
    uint32_t       rx_ms;      // the last well-formed KEYBOARD_UPDATE request
    bool           polled;     // a well-formed request since the last sector erase (ERASING)
    uint8_t        filled;     // manifest bytes received
    uint32_t       image_len;  // from the checked manifest (ERASING on)
    uint32_t       erase_off, erase_end;
    uint32_t       crc_body; // VERIFIED: CRC-32/MPEG-2 over [0x100, image_len)
    uint32_t       commit_due_ms;
    uint8_t        manifest[UPDATE_MANIFEST_BYTES] __attribute__((aligned(4)));
    usplit_pages_t pages; // RECEIVING: the page being assembled
    // SETTINGS_RESET (D42): the build reset record is invalidated; reset at reset_due_ms.
    bool     reset_pending;
    uint32_t reset_due_ms;
} sl;

static struct {
    bool              have; // manifest is the copy for mailbox sequence mseq
    uint32_t          mseq;
    bool              verifying; // VERIFYING_IMAGE has begun: sha, verify_off and crc are live
    uint32_t          verify_off;
    uint32_t          crc;
    crypto_sha512_ctx sha;
    update_build_scan_t bi; // the image's build-info records (D45)
    uint8_t           manifest[UPDATE_MANIFEST_BYTES] __attribute__((aligned(4)));
} hk;

#define SL_MANIFEST ((const sval_update_manifest_t *)hk.manifest)

uint8_t update_split_slave_state(void) {
    return sl.state;
}

bool update_split_slave_active(void) {
    return sl.state != UPDATE_STATE_IDLE;
}

// To IDLE, with last_error kept as the reason. Never erases (as the USB
// ABORT): BEGIN erases anyway. Callback, or housekeeping holding the lock.
static void slave_cancel(uint8_t error) {
    sl.state      = UPDATE_STATE_IDLE;
    sl.last_error = error;
    sl.mseq++;
    sl.filled    = 0;
    sl.image_len = sl.erase_off = sl.erase_end = sl.crc_body = 0;
    memset(sl.manifest, 0, sizeof(sl.manifest));
    memset(&sl.pages, 0, sizeof(sl.pages));
}

static update_status_t slave_fail(update_status_t st) {
    sl.state      = UPDATE_STATE_ERROR;
    sl.last_error = st;
    return st;
}

static update_status_t slave_begin(const usplit_req_t *r, uint8_t *payload) {
    if (r->off == 0) {
        // A new session, from IDLE or over a half-sent manifest only.
        if (sl.state != UPDATE_STATE_IDLE && sl.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
        if (!update_flash_available()) return UPDATE_UNAVAILABLE; // probed at init, on the main thread
        slave_cancel(UPDATE_OK);
        sl.state = UPDATE_STATE_MANIFEST_LOADING;
    } else if (sl.state != UPDATE_STATE_MANIFEST_LOADING) {
        // A repeat of a fragment already taken is acknowledged; nothing else.
        bool repeat = sl.state != UPDATE_STATE_IDLE && sl.state != UPDATE_STATE_ERROR && r->off + r->n <= sl.filled;
        payload[0]  = sl.filled;
        return repeat ? UPDATE_OK : UPDATE_INVALID;
    } else if (r->off + r->n <= sl.filled) {
        payload[0] = sl.filled; // a repeat: acknowledged, not written again
        return UPDATE_OK;
    } else if (r->off != sl.filled) {
        payload[0] = sl.filled;
        return UPDATE_OUT_OF_ORDER;
    }
    memcpy(sl.manifest + r->off, r->data, r->n);
    sl.filled  = (uint8_t)(r->off + r->n);
    payload[0] = sl.filled;
    // The last fragment posts the first job: housekeeping copies the manifest
    // out, checks it itself (D7), then erases.
    if (sl.filled == UPDATE_MANIFEST_BYTES) sl.state = UPDATE_STATE_VERIFYING_MANIFEST;
    return UPDATE_OK;
}

static update_status_t slave_page(const usplit_req_t *r, uint8_t *payload) {
    if (sl.state == UPDATE_STATE_VERIFYING_MANIFEST || sl.state == UPDATE_STATE_ERASING) {
        put24(payload, 0);
        return UPDATE_BUSY;
    }
    if (sl.state != UPDATE_STATE_RECEIVING) {
        put24(payload, sl.state == UPDATE_STATE_IDLE ? 0 : sl.pages.next);
        return UPDATE_INVALID;
    }
    uint32_t        done;
    update_status_t st = usplit_pages_accept(&sl.pages, r->off, r->data, r->n, &done);
    put24(payload, sl.pages.next);
    if (st == UPDATE_OVERRUN) return slave_fail(UPDATE_OVERRUN);
    if (st != UPDATE_OK) return st; // OUT_OF_ORDER or INVALID: refused, nothing changes
    if (done != UINT32_MAX) {
        // One page, here in the SlaveThread while the master waits for the
        // answer (R16): no interrupts-off window races the link. This thread
        // can preempt the main thread in the middle of a DMA transfer (the PMW
        // SPI's TX source is in XIP, R6), so the port refuses with BUSY while
        // any DMA channel is busy, checked in the same interrupts-off section
        // as the program. Then the fragment is given back (nothing was
        // written) and the master sends it again in a new request.
        update_status_t f = update_split_port_program_page(done, sl.pages.page);
        if (f == UPDATE_BUSY) {
            sl.pages.next = r->off;
            put24(payload, sl.pages.next);
            return UPDATE_BUSY;
        }
        if (f != UPDATE_OK) return slave_fail(f == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
    }
    return UPDATE_OK;
}

static void slave_status(uint8_t *payload) {
    bool rx = sl.state >= UPDATE_STATE_RECEIVING && sl.state <= UPDATE_STATE_COMMITTING;
    payload[0] = sl.last_error;
    put24(&payload[1], rx ? sl.pages.next : 0);
    put16(&payload[4], sl.erase_off / UPDATE_FLASH_SECTOR);
    put16(&payload[6], sl.erase_end / UPDATE_FLASH_SECTOR);
    put16(&payload[8], sl.state == UPDATE_STATE_VERIFIED || sl.state == UPDATE_STATE_COMMITTING ? sl.crc_body & 0xFFFF : 0);
    payload[10] = sl.state != UPDATE_STATE_IDLE ? 0x01 : 0;
}

// A request that passed usplit_req_parse. Fills the op's response payload.
static update_status_t run(const usplit_req_t *r, uint8_t *payload) {
    switch (r->op) {
        case USPLIT_OP_INFO: {
            update_build_id_t id;
            update_split_port_build_id(&id);
            payload[0] = id.updater_proto;
            payload[1] = id.hand;
            payload[2] = id.pointing_id;
            put32(&payload[3], id.fw_version);
            put32(&payload[7], id.git_hash);
            payload[11] = SVAL_UPDATE_STORAGE_FORMAT;
            put16(&payload[12], SVAL_UPDATE_SECURITY_EPOCH);
            payload[14] = update_split_port_info_flags();
            return UPDATE_OK;
        }
        case USPLIT_OP_BEGIN:
            return slave_begin(r, payload);
        case USPLIT_OP_PAGE:
            return slave_page(r, payload);
        case USPLIT_OP_STATUS:
            slave_status(payload);
            return UPDATE_OK;
        case USPLIT_OP_END:
            // A repeat (its answer lost, and the retry cache since overwritten) is acknowledged again.
            if (sl.state == UPDATE_STATE_VERIFYING_IMAGE || sl.state == UPDATE_STATE_VERIFIED) return UPDATE_ACCEPTED;
            if (sl.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
            if (sl.pages.next != sl.image_len) return UPDATE_OUT_OF_ORDER; // END before the last byte
            sl.state = UPDATE_STATE_VERIFYING_IMAGE;                     // housekeeping: SHA-512, structure, CRC
            return UPDATE_ACCEPTED;
        case USPLIT_OP_COMMIT:
            if (sl.state == UPDATE_STATE_COMMITTING && r->crc_lo16 == (sl.crc_body & 0xFFFF)) return UPDATE_ACCEPTED; // a repeat: one commit
            if (sl.state != UPDATE_STATE_VERIFIED) return UPDATE_INVALID;
            if (r->crc_lo16 != (sl.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
            if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;     // D12
            if (!update_commit_available()) return UPDATE_UNSUPPORTED;
            // Housekeeping runs the commit (update_commit.c, unchanged) behind
            // the master's pause, which the master took before sending this.
            sl.state         = UPDATE_STATE_COMMITTING;
            sl.commit_due_ms = update_split_port_now_ms() + SLAVE_COMMIT_DELAY_MS;
            return UPDATE_ACCEPTED;
        case USPLIT_OP_SETTINGS_RESET: {
            // D42: the master asks this half first, then does the same itself.
            if (sl.reset_pending) return UPDATE_OK; // a repeat whose answer was lost
            if (sl.state != UPDATE_STATE_IDLE) return UPDATE_BUSY;
            // One page program in this thread, as PAGE does (R16), under the
            // same DMA check: BUSY means nothing was written, and the master
            // sends a new request.
            update_status_t st = update_split_port_settings_invalidate(r->mode);
            if (st != UPDATE_OK) return st;
            sl.reset_pending = true;
            sl.reset_due_ms  = update_split_port_now_ms() + SVAL_UPDATE_SETTINGS_RESET_SLAVE_MS;
            return UPDATE_OK;
        }
        default: // USPLIT_OP_ABORT
            if (sl.state == UPDATE_STATE_COMMITTING) return UPDATE_BUSY; // the copy may have started (D23)
            if (sl.state != UPDATE_STATE_IDLE) slave_cancel(UPDATE_OK);
            return UPDATE_OK;
    }
}

void update_split_slave_rpc(const uint8_t *in, uint8_t in_len, uint8_t *out, uint8_t out_len) {
    // The lengths come from the wire (rpc_info, behind only a CRC-8): never
    // past the 32-byte RPC buffers.
    if (out_len > USPLIT_MSG_MAX) out_len = USPLIT_MSG_MAX;
    if (in_len > USPLIT_MSG_MAX) in_len = USPLIT_MSG_MAX;
    memset(out, 0, out_len);
    if (in_len < USPLIT_REQ_HDR) return; // no op and seq to answer to
    usplit_req_t    r;
    update_status_t st = usplit_req_parse(in, in_len, &r);
    if (st == UPDATE_OK) {
        sl.rx_ms  = update_split_port_now_ms(); // traffic: the session's timeout restarts
        sl.polled = true;                       // and the next sector may be erased (ERASING)
    }
    if (st == UPDATE_OK && sl.cached && r.seq == sl.seq && r.crc == sl.crc) {
        // A retry of the last request (its answer was lost): the same answer,
        // and the request is not run again.
        if (out_len >= sl.rsp_len) memcpy(out, sl.rsp, sl.rsp_len);
        return;
    }
    uint8_t payload[USPLIT_MSG_MAX];
    memset(payload, 0, sizeof(payload));
    bool valid = st == UPDATE_OK;
    if (valid) st = run(&r, payload);
    uint8_t rsp[USPLIT_MSG_MAX];
    // An unknown op (or a frame too short to name one) builds nothing: the
    // master's check of the reply fails and it retries.
    uint8_t len = usplit_rsp_build(rsp, sizeof(rsp), (uint8_t)st, r.op, r.seq, sl.state, valid ? payload : NULL);
    if (!len) return;
    if (valid) {
        sl.cached  = true;
        sl.seq     = r.seq;
        sl.crc     = r.crc;
        sl.rsp_len = len;
        memcpy(sl.rsp, rsp, len);
    }
    if (out_len >= len) memcpy(out, rsp, len);
}

// ---- the slave's housekeeping ----------------------------------------------------------

// Posts a job's result if the session is still where the job found it (same
// mailbox sequence number and state): else an ABORT, the timeout or a new
// session won the race, and the result is dropped.
static bool post_begin(uint32_t mseq, uint8_t state) {
    update_split_port_lock();
    if (sl.mseq == mseq && sl.state == state) return true; // the caller posts, then unlocks
    update_split_port_unlock();
    return false;
}

static void slave_check_manifest(uint32_t mseq) {
    update_device_t dev;
    update_device_self(&dev);
    // This half checks the manifest itself (D7).
    update_status_t st = updater_port_settings_failing() ? UPDATE_UNAVAILABLE : update_manifest_check(SL_MANIFEST, &dev);
    if (!post_begin(mseq, UPDATE_STATE_VERIFYING_MANIFEST)) return;
    if (st != UPDATE_OK) {
        slave_fail(st);
    } else {
        sl.image_len = SL_MANIFEST->image_len;
        sl.erase_off = 0;
        sl.erase_end = (sl.image_len + UPDATE_FLASH_SECTOR - 1) / UPDATE_FLASH_SECTOR * UPDATE_FLASH_SECTOR;
        sl.state     = UPDATE_STATE_ERASING;
    }
    update_split_port_unlock();
}

// One sector per pass, as the M1 path; sectors already erased are skipped.
// A sector erase keeps this half's interrupts off for about 50 ms, longer
// than the master's 20 ms serial timeout, so this half cannot answer while it
// erases. So a sector that needs erasing waits for a request from the master
// since the last erase (gate: polled, and SVAL_UPDATE_SPLIT_ERASE_GAP_MS
// since it, for the rest of that RPC's transactions): the master gets an
// answer between every two sectors, its link timeout never races the erase,
// and an ABORT always lands within a sector.
static void slave_erase(uint32_t mseq, uint32_t off, bool gate) {
    if (!gate && !update_flash_sector_erased(off)) return;
    update_status_t st = update_flash_erase_sector(off);
    if (!post_begin(mseq, UPDATE_STATE_ERASING)) return;
    sl.polled = false;
    if (sl.erase_off == off) {
        if (st != UPDATE_OK) {
            slave_fail(st == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
        } else {
            sl.erase_off += UPDATE_FLASH_SECTOR;
            if (sl.erase_off >= sl.erase_end) {
                usplit_pages_init(&sl.pages, sl.image_len);
                sl.state = UPDATE_STATE_RECEIVING;
            }
        }
    }
    update_split_port_unlock();
}

static void slave_verify(uint32_t mseq) {
    uint32_t len = SL_MANIFEST->image_len;
    if (!hk.verifying) {
        crypto_sha512_init(&hk.sha);
        update_build_scan_init(&hk.bi);
        hk.verify_off = 0;
        hk.crc        = UPDATE_CRC32_INIT;
        hk.verifying  = true;
    }
    uint8_t         buf[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
    update_status_t st  = UPDATE_OK;
    uint32_t        end = len - hk.verify_off > SLAVE_VERIFY_SLICE ? hk.verify_off + SLAVE_VERIFY_SLICE : len;
    while (hk.verify_off < end) {
        uint32_t n = end - hk.verify_off < sizeof(buf) ? end - hk.verify_off : sizeof(buf);
        if (!slot_copy(hk.verify_off, buf, n)) {
            st = UPDATE_FLASH_ERR;
            break;
        }
        crypto_sha512_update(&hk.sha, buf, n);
        update_build_scan(&hk.bi, buf, n);
        uint32_t skip = hk.verify_off < UPDATE_VECTORS_OFFSET ? UPDATE_VECTORS_OFFSET - hk.verify_off : 0;
        if (skip < n) hk.crc = update_crc32_mpeg2_update(hk.crc, buf + skip, n - skip);
        hk.verify_off += n;
    }
    if (st == UPDATE_OK && hk.verify_off < len) return; // more next pass
    if (st == UPDATE_OK) {
        uint8_t hash[UPDATE_SHA512_BYTES];
        crypto_sha512_final(&hk.sha, hash);
        st = memcmp(hash, SL_MANIFEST->sha512, UPDATE_SHA512_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_HASH;
    }
    if (st == UPDATE_OK) {
        uint8_t head[UPDATE_HEAD_BYTES] __attribute__((aligned(4)));
        st = slot_copy(0, head, sizeof(head)) ? update_image_head_check(head, len) : UPDATE_FLASH_ERR;
    }
    if (st == UPDATE_OK) {
        update_device_t dev;
        update_device_self(&dev);
        st = update_image_build_check(&hk.bi, &dev); // D45: this half checks the image itself too (D7)
    }
    hk.verifying = false;
    if (!post_begin(mseq, UPDATE_STATE_VERIFYING_IMAGE)) return;
    if (st != UPDATE_OK) {
        slave_fail(st);
    } else {
        sl.crc_body = hk.crc;
        sl.state    = UPDATE_STATE_VERIFIED;
    }
    update_split_port_unlock();
}

static void slave_commit(uint32_t mseq) {
    if (!post_begin(mseq, UPDATE_STATE_COMMITTING)) return;
    // Image bytes in RAM: the page buffer goes now (the commit sweeps the
    // double-tap magic from the rest of RAM itself, R23).
    memset(&sl.pages, 0, sizeof(sl.pages));
    update_split_port_unlock();
    memset(&hk.sha, 0, sizeof(hk.sha));
    // The M1 commit, unchanged: step 0 checks the manifest against this build
    // and the slot's SHA-512 and CRC again. It returns only when it refused
    // before touching the firmware.
    updater_port_commit_mark(true); // the next boot shows the rainbow until the halves match (D24)
    update_status_t st = update_commit_run(SL_MANIFEST, hk.crc);
    updater_port_commit_mark(false);
    if (!post_begin(mseq, UPDATE_STATE_COMMITTING)) return;
    slave_fail(st != UPDATE_OK ? st : UPDATE_FLASH_ERR);
    update_split_port_unlock();
}

bool update_split_slave_reset_pending(void) {
    return sl.reset_pending;
}

void update_split_slave_task(void) {
    // SETTINGS_RESET (D42): the record is invalidated; the next boot resets
    // every stored setting (D39). Set once by the callback, never cleared.
    if (sl.reset_pending && due(update_split_port_now_ms(), sl.reset_due_ms)) {
        update_split_port_reboot();
        return; // host tests: the simulation reboots this half
    }
    if (sl.state == UPDATE_STATE_IDLE) return; // a byte read: the common case takes no lock
    update_split_port_lock();
    // The clock is read under the lock: read before it, the SlaveThread could
    // set rx_ms to a later millisecond in between, and now - rx_ms would wrap.
    // Compared signed as well, as due() does.
    uint32_t now   = update_split_port_now_ms();
    uint8_t  state = sl.state;
    uint32_t mseq  = sl.mseq;
    if (state == UPDATE_STATE_IDLE) {
        update_split_port_unlock();
        return;
    }
    // No KEYBOARD_UPDATE traffic for 5 s: the master is gone (link pulled, or
    // it failed and its ABORT was lost). Drop the session, keeping any error
    // as the reason; the LEDs go back to normal. Not once COMMIT was accepted.
    if (state != UPDATE_STATE_COMMITTING && (int32_t)(now - sl.rx_ms) >= SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS) {
        slave_cancel(state == UPDATE_STATE_ERROR ? sl.last_error : UPDATE_TIMEOUT);
        update_split_port_unlock();
        return;
    }
    // The mailbox: the manifest is copied out once per session, here, under
    // the lock; everything after works on the copy.
    if (state == UPDATE_STATE_VERIFYING_MANIFEST && !(hk.have && hk.mseq == mseq)) {
        memcpy(hk.manifest, sl.manifest, sizeof(hk.manifest));
        hk.have      = true;
        hk.mseq      = mseq;
        hk.verifying = false;
    }
    bool     mine       = hk.have && hk.mseq == mseq;
    uint32_t erase_off  = sl.erase_off;
    bool     erase_gate = sl.polled && (int32_t)(now - sl.rx_ms) >= SVAL_UPDATE_SPLIT_ERASE_GAP_MS;
    bool     commit_now = state == UPDATE_STATE_COMMITTING && due(now, sl.commit_due_ms);
    bool     lost       = !mine && (state == UPDATE_STATE_ERASING || state == UPDATE_STATE_RECEIVING || state == UPDATE_STATE_VERIFYING_IMAGE || state == UPDATE_STATE_VERIFIED || state == UPDATE_STATE_COMMITTING);
    if (lost) slave_fail(UPDATE_INVALID); // cannot happen: the copy is taken before ERASING
    update_split_port_unlock();
    if (lost) return;

    switch (state) {
        case UPDATE_STATE_VERIFYING_MANIFEST:
            slave_check_manifest(mseq);
            break;
        case UPDATE_STATE_ERASING:
            slave_erase(mseq, erase_off, erase_gate);
            break;
        case UPDATE_STATE_VERIFYING_IMAGE:
            slave_verify(mseq);
            break;
        case UPDATE_STATE_COMMITTING:
            if (commit_now) slave_commit(mseq);
            break;
        default:
            break;
    }
}

// ---- the relay, the half with USB -----------------------------------------------------

#define RELAY_RESYNC_MAX 8
// A request that arrives damaged is answered INVALID with its op and seq (when
// those survived) and is not cached, so the same frame sent again runs; a
// well-formed request the other half refuses as INVALID is cached and gets
// INVALID again. So INVALID is taken as a refusal only on the fourth answer
// in a row to the same frame.
#define RELAY_INVALID_TRIES 4
// The probe after the hold finds the other half still COMMITTING (it answers,
// so its interrupts are on and its copy has not begun): probe again every
// SVAL_UPDATE_RELAY_PROBE_MS, still paused, at most this many times in all.
#define RELAY_PROBES_MAX 20

static uint8_t master_seq; // every new request gets the next one (retries keep theirs)

static struct {
    uint8_t         phase; // relay_phase_t
    update_status_t error;
    uint8_t         manifest[UPDATE_MANIFEST_BYTES];
    uint32_t        image_len, crc_body;
    uint32_t        off;   // RELAY_MANIFEST: manifest bytes taken; RELAY_PAGES: next image byte to send
    uint8_t         req[USPLIT_MSG_MAX];
    uint8_t         req_len, req_n;
    bool            pending; // req is out (or to go out again) without a valid answer
    bool            paused;
    uint32_t        start_ms, end_ms, last_ok_ms, next_ms, hold_until_ms;
    uint8_t         slave_state, slave_error;
    uint16_t        slave_erased, slave_to_erase;
    uint32_t        acked;
    uint16_t        retries, resyncs;
    uint8_t         invalids; // INVALID answers in a row to the pending frame
    uint8_t         abort_to; // RELAY_ABORTING ends in this phase (RELAY_FAILED or RELAY_IDLE)
    uint8_t         probes;   // RELAY_HOLD: probes so far
    bool            saw_committing; // the last probe found the other half COMMITTING
    bool            unconfirmed;    // DONE without an answer after COMMIT (the probe went unanswered)
    uint32_t        page_off; // the slot page in page[], or UINT32_MAX
    bool            from_self; // the boot relay (update_pair.c): this half's own image, not the slot
    uint8_t         page[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
} rl;

bool update_relay_busy(void) {
    return rl.phase != RELAY_IDLE && rl.phase != RELAY_DONE && rl.phase != RELAY_FAILED;
}

static bool relay_pause(void) {
    if (rl.paused) return true;
    if (!update_split_port_pause(true)) return false;
    rl.paused = true;
    return true;
}

static void relay_resume(void) {
    if (!rl.paused) return;
    update_split_port_pause(false);
    rl.paused = false;
}

static bool relay_request(uint8_t len, uint8_t n) {
    rl.req_len  = len;
    rl.req_n    = n;
    rl.pending  = len != 0;
    rl.invalids = 0;
    return len != 0;
}

static bool relay_new(uint8_t op) {
    return relay_request(usplit_req_build(rl.req, op, ++master_seq, NULL, 0), 0);
}

static void relay_send_abort(void) {
    uint8_t req[USPLIT_MSG_MAX], rsp[USPLIT_MSG_MAX];
    uint8_t len = usplit_req_build(req, USPLIT_OP_ABORT, ++master_seq, NULL, 0);
    update_split_port_rpc(req, len, rsp, usplit_rsp_len(USPLIT_OP_ABORT)); // one try: the other half times out anyway
}

static void relay_finish(void) {
    rl.phase   = rl.abort_to;
    rl.pending = false;
    rl.end_ms  = update_split_port_now_ms();
    relay_resume();
}

// Ends the relay in phase to (RELAY_FAILED or RELAY_IDLE). The other half is
// told to drop its session first, and the link (if paused) stays paused until
// it has answered the ABORT (RELAY_ABORTING, one try per pass), or for at most
// SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS: it may be in the middle of its erase,
// which must not run with the link live (D15). Only when the link is already
// dead (no valid answer for the link timeout) is there one try and no wait.
static void relay_stop(uint8_t to, bool link_dead) {
    rl.abort_to = to;
    if (link_dead) {
        relay_send_abort();
        relay_finish();
        return;
    }
    rl.phase      = RELAY_ABORTING;
    rl.last_ok_ms = update_split_port_now_ms();
    if (!relay_new(USPLIT_OP_ABORT)) relay_finish();
}

static void relay_fail_how(update_status_t st, bool link_dead) {
    if (!update_relay_busy() || rl.phase == RELAY_ABORTING) return;
    rl.error = st;
    relay_stop(RELAY_FAILED, link_dead);
}

static void relay_fail(update_status_t st) {
    relay_fail_how(st, false);
}

// The other half's commit and reboot: the link stays paused for both (R18),
// then the probe decides.
static void relay_hold(uint32_t now) {
    rl.phase         = RELAY_HOLD;
    rl.pending       = false;
    rl.hold_until_ms = now + SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS;
}

// No valid answer for SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS.
static void relay_timeout(uint32_t now) {
    if (rl.phase == RELAY_ABORTING) {
        relay_finish(); // the other half drops its session by its own timeout
    } else if (rl.phase == RELAY_COMMIT) {
        // COMMIT may have been taken with its answer lost, and the other half
        // may be writing now (it cannot answer then): not resumed, not a
        // failure. The hold, then the probe decides.
        relay_hold(now);
    } else {
        relay_fail_how(UPDATE_UNAVAILABLE, true);
    }
}

// The other half answered in a state the relay did not expect: its error if
// it latched one, else it lost the session (it rebooted, or timed out).
static void relay_lost(void) {
    relay_fail(rl.slave_error != UPDATE_OK ? (update_status_t)rl.slave_error : UPDATE_UNAVAILABLE);
}

// Sends the pending request once. True with *r filled on a valid answer; on
// none, the request stays pending for a retry, and the relay fails once there
// has been no valid answer for SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS.
static bool relay_exchange(usplit_rsp_t *r, uint8_t *buf) {
    uint8_t op   = rl.req[0];
    uint8_t want = usplit_rsp_len(op);
    memset(buf, 0, USPLIT_MSG_MAX);
    bool     ok  = update_split_port_rpc(rl.req, rl.req_len, buf, want) && usplit_rsp_parse(buf, want, op, rl.req[1], r) == UPDATE_OK;
    uint32_t now = update_split_port_now_ms();
    if (ok && r->status == UPDATE_INVALID && ++rl.invalids < RELAY_INVALID_TRIES) ok = false; // maybe damaged on the way: again
    if (ok) {
        rl.pending     = false;
        rl.last_ok_ms  = now;
        rl.slave_state = r->state;
        return true;
    }
    rl.retries++;
    if ((int32_t)(now - rl.last_ok_ms) >= SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS) relay_timeout(now);
    return false;
}

static void relay_status_payload(const usplit_rsp_t *r) {
    rl.slave_error    = r->payload[0];
    rl.slave_erased   = get16(&r->payload[4]);
    rl.slave_to_erase = get16(&r->payload[6]);
}

// One request and its answer at most. True when the relay may go straight on
// in this pass.
static bool relay_step(void) {
    uint8_t      buf[USPLIT_MSG_MAX];
    usplit_rsp_t r;
    uint32_t     now = update_split_port_now_ms();
    memset(&r, 0, sizeof(r)); // a request that could not be built fails as INVALID below
    switch (rl.phase) {
        case RELAY_START:
            // An old session on the other half (a failed relay whose ABORT was
            // lost) would refuse BEGIN: clear it first. ABORT never erases.
            if (!rl.pending && !relay_new(USPLIT_OP_ABORT)) break;
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            rl.phase = RELAY_MANIFEST;
            rl.off   = 0;
            return true;

        case RELAY_MANIFEST: {
            if (!rl.pending) {
                uint8_t n = UPDATE_MANIFEST_BYTES - rl.off < USPLIT_BEGIN_DATA_MAX ? (uint8_t)(UPDATE_MANIFEST_BYTES - rl.off) : USPLIT_BEGIN_DATA_MAX;
                if (!relay_request(usplit_req_begin(rl.req, ++master_seq, (uint8_t)rl.off, rl.manifest + rl.off, n), n)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            if (r.payload[0] != rl.off + rl.req_n) {
                relay_fail(UPDATE_INVALID);
                return false;
            }
            rl.off = r.payload[0];
            if (rl.off < UPDATE_MANIFEST_BYTES) return true;
            // The other half now checks the manifest and erases, behind the pause.
            rl.phase   = RELAY_ERASE_WAIT;
            rl.next_ms = now + SVAL_UPDATE_RELAY_POLL_MS;
            return false;
        }

        case RELAY_ERASE_WAIT:
        case RELAY_VERIFY_WAIT:
        case RELAY_VERIFIED:
            if (!rl.pending) {
                if (!due(now, rl.next_ms)) return false;
                if (!relay_new(USPLIT_OP_STATUS)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            relay_status_payload(&r);
            rl.next_ms = now + (rl.phase == RELAY_VERIFIED ? SVAL_UPDATE_RELAY_KEEPALIVE_MS : SVAL_UPDATE_RELAY_POLL_MS);
            if (r.state == UPDATE_STATE_ERROR) {
                relay_lost();
                return false;
            }
            if (rl.phase == RELAY_ERASE_WAIT) {
                if (r.state == UPDATE_STATE_VERIFYING_MANIFEST || r.state == UPDATE_STATE_ERASING) return false;
                if (r.state != UPDATE_STATE_RECEIVING) {
                    relay_lost();
                    return false;
                }
                // Erased: the image goes over with the link running, so both
                // halves keep typing between the RPCs.
                relay_resume();
                rl.phase = RELAY_PAGES;
                rl.off   = 0;
                return true;
            }
            if (rl.phase == RELAY_VERIFY_WAIT) {
                if (r.state == UPDATE_STATE_VERIFYING_IMAGE) return false;
                if (r.state != UPDATE_STATE_VERIFIED) {
                    relay_lost();
                    return false;
                }
                if (get16(&r.payload[8]) != (rl.crc_body & 0xFFFF)) {
                    relay_fail(UPDATE_BAD_HASH); // it verified some other image
                    return false;
                }
                relay_resume();
                rl.phase   = RELAY_VERIFIED;
                rl.next_ms = now + SVAL_UPDATE_RELAY_KEEPALIVE_MS;
                return false;
            }
            if (r.state != UPDATE_STATE_VERIFIED) relay_lost(); // RELAY_VERIFIED: still holding it?
            return false;

        case RELAY_PAGES: {
            if (!rl.pending) {
                if (rl.off >= rl.image_len) {
                    // All acknowledged: the other half verifies behind the pause.
                    if (!relay_pause()) {
                        relay_fail(UPDATE_UNAVAILABLE);
                        return false;
                    }
                    if (!relay_new(USPLIT_OP_END)) break;
                    rl.phase = RELAY_END;
                    return true;
                }
                uint32_t base = rl.off - rl.off % UPDATE_FLASH_PAGE;
                if (rl.page_off != base) {
                    bool read = rl.from_self ? update_split_port_self_read(base, rl.page, UPDATE_FLASH_PAGE) : update_split_port_slot_read(base, rl.page, UPDATE_FLASH_PAGE);
                    if (!read) {
                        relay_fail(UPDATE_FLASH_ERR);
                        return false;
                    }
                    rl.page_off = base;
                }
                uint32_t at = rl.off % UPDATE_FLASH_PAGE;
                uint32_t n  = UPDATE_FLASH_PAGE - at < USPLIT_PAGE_DATA_MAX ? UPDATE_FLASH_PAGE - at : USPLIT_PAGE_DATA_MAX;
                if (n > rl.image_len - rl.off) n = rl.image_len - rl.off;
                if (!relay_request(usplit_req_page(rl.req, ++master_seq, rl.off, rl.page + at, (uint8_t)n), (uint8_t)n)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            uint32_t next = get24(r.payload);
            if (r.status == UPDATE_OK && next >= rl.off + rl.req_n && next <= rl.image_len) {
                rl.off   = next;
                rl.acked = next;
                return true;
            }
            if (r.status == UPDATE_OUT_OF_ORDER && next < rl.image_len && rl.resyncs < RELAY_RESYNC_MAX) {
                rl.resyncs++; // the other half holds [0, next): carry on from there
                rl.off = next;
                return true;
            }
            if (r.status == UPDATE_BUSY) return false; // a new request next pass
            if (r.state == UPDATE_STATE_IDLE) {
                relay_fail(UPDATE_UNAVAILABLE); // it lost the session: rebooted, or timed out
                return false;
            }
            relay_fail(r.status == UPDATE_OK ? UPDATE_INVALID : (update_status_t)r.status);
            return false;
        }

        case RELAY_END:
            if (!rl.pending && !relay_new(USPLIT_OP_END)) break;
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_ACCEPTED) break;
            rl.phase   = RELAY_VERIFY_WAIT;
            rl.next_ms = now + SVAL_UPDATE_RELAY_POLL_MS;
            return false;

        case RELAY_COMMIT:
            // The request was built by update_relay_commit(); a retry keeps it.
            // No valid answer for the link timeout: the hold (relay_timeout).
            if (!relay_exchange(&r, buf)) return false;
            // ACCEPTED: the other half commits and resets. IDLE: it already
            // did (the answer to the first COMMIT was lost, and the retry
            // reached its new image), or it lost the session; the probe after
            // the hold tells which. Either way the link stays paused so this
            // half does not stall on a silent link (R18), and is not resumed
            // in the middle of the other half's commit.
            if (r.status != UPDATE_ACCEPTED && r.state != UPDATE_STATE_IDLE) break;
            relay_hold(now);
            return false;

        case RELAY_HOLD: {
            if (!due(now, rl.hold_until_ms)) return false;
            // The probe. A half that reset into its new image answers IDLE
            // with no error, or (with a new split table) not at all; one that
            // refused the commit is still in ERROR, or dropped it by timeout;
            // one still COMMITTING has not begun its copy yet.
            rl.phase = RELAY_PROBE;
            if (!relay_new(USPLIT_OP_STATUS)) break;
            uint8_t want = usplit_rsp_len(USPLIT_OP_STATUS);
            memset(buf, 0, sizeof(buf));
            bool answered = update_split_port_rpc(rl.req, rl.req_len, buf, want) && usplit_rsp_parse(buf, want, USPLIT_OP_STATUS, rl.req[1], &r) == UPDATE_OK;
            rl.pending = false;
            rl.probes++;
            if (answered) {
                rl.slave_state = r.state;
                relay_status_payload(&r);
                if (r.state == UPDATE_STATE_COMMITTING) {
                    // Not DONE: it has still to write. Probe again soon, still paused.
                    if (rl.probes >= RELAY_PROBES_MAX) {
                        relay_fail(UPDATE_TIMEOUT);
                        return false;
                    }
                    rl.saw_committing = true;
                    rl.phase          = RELAY_HOLD;
                    rl.hold_until_ms  = now + SVAL_UPDATE_RELAY_PROBE_MS;
                    return false;
                }
                bool failed = r.state != UPDATE_STATE_IDLE || r.payload[0] != UPDATE_OK;
                if (failed) {
                    relay_lost();
                    return false;
                }
            } else if (rl.saw_committing) {
                // COMMITTING at the last probe and silent now: it has begun
                // its copy (interrupts off). The whole hold again.
                if (rl.probes >= RELAY_PROBES_MAX) {
                    relay_fail(UPDATE_TIMEOUT);
                    return false;
                }
                rl.saw_committing = false;
                relay_hold(now);
                return false;
            } else {
                rl.unconfirmed = true; // a new release may not talk to this one (V): the host checks presence
            }
            rl.phase       = RELAY_DONE;
            rl.end_ms      = update_split_port_now_ms();
            awaiting_match = true;
            relay_resume();
            return false;
        }

        case RELAY_ABORTING:
            // relay_stop(): ABORT until the other half answers (it then holds
            // no session, or refuses as BUSY while COMMITTING), or for the
            // link timeout (relay_timeout); then the link resumes.
            if (!rl.pending && !relay_new(USPLIT_OP_ABORT)) {
                relay_finish();
                return false;
            }
            if (!relay_exchange(&r, buf)) return false;
            relay_finish();
            return false;

        default:
            return false;
    }
    // A refusal: the other half's status, as the relay's error.
    relay_fail(r.status != UPDATE_OK && r.status != UPDATE_ACCEPTED ? (update_status_t)r.status : UPDATE_INVALID);
    return false;
}

static bool relay_start(const uint8_t *manifest, uint32_t image_len, uint32_t crc_body, bool from_self) {
    if (update_relay_busy()) return false;
    relay_resume();
    memset(&rl, 0, sizeof(rl));
    memcpy(rl.manifest, manifest, sizeof(rl.manifest));
    rl.image_len = image_len;
    rl.crc_body  = crc_body;
    rl.page_off  = UINT32_MAX;
    rl.from_self = from_self;
    rl.start_ms = rl.last_ok_ms = update_split_port_now_ms();
    // Paused from the start: the other half's manifest check and erase come
    // straight after the manifest.
    if (!relay_pause()) return false;
    rl.phase = RELAY_START;
    return true;
}

bool update_relay_start(const uint8_t *manifest, uint32_t image_len, uint32_t crc_body) {
    return relay_start(manifest, image_len, crc_body, false);
}

bool update_relay_start_self(const uint8_t *manifest, uint32_t image_len, uint32_t crc_body) {
    return relay_start(manifest, image_len, crc_body, true);
}

void update_relay_task(void) {
    if (!update_relay_busy()) return;
    uint32_t t0 = update_split_port_now_ms();
    while (relay_step() && update_relay_busy() && update_split_port_now_ms() - t0 < SVAL_UPDATE_RELAY_PASS_MS) {
    }
}

bool update_relay_commit(void) {
    if (rl.phase != RELAY_VERIFIED) return false;
    if (!relay_pause()) return false;
    // A keepalive still waiting for its answer is dropped: COMMIT replaces it.
    if (!relay_request(usplit_req_commit(rl.req, ++master_seq, (uint16_t)(rl.crc_body & 0xFFFF)), 0)) {
        relay_resume();
        return false;
    }
    rl.phase = RELAY_COMMIT;
    // COMMIT gets the whole link timeout, not what is left of the keepalive's.
    rl.last_ok_ms = update_split_port_now_ms();
    return true;
}

void update_relay_abort(void) {
    if (rl.phase == RELAY_COMMIT || rl.phase == RELAY_HOLD || rl.phase == RELAY_PROBE) return; // COMMIT is out
    if (rl.phase == RELAY_ABORTING) return;                                                       // already stopping
    if (update_relay_busy()) {
        relay_stop(RELAY_IDLE, false); // the link resumes once the other half has answered the ABORT
        return;
    }
    relay_resume(); // a finished relay (DONE, FAILED) keeps its record for the RELAY op
}

bool update_split_awaiting_match(void) {
    return awaiting_match;
}

void update_relay_info(update_relay_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->phase          = rl.phase;
    info->error          = rl.error;
    info->slave_state    = rl.slave_state;
    info->slave_error    = rl.slave_error;
    info->acked          = rl.acked;
    info->image_len      = rl.image_len;
    info->slave_erased   = rl.slave_erased;
    info->slave_to_erase = rl.slave_to_erase;
    info->retries        = rl.retries;
    info->elapsed_ms     = rl.phase == RELAY_IDLE ? 0 : (update_relay_busy() ? update_split_port_now_ms() : rl.end_ms) - rl.start_ms;
    info->paused         = rl.paused;
    info->unconfirmed    = rl.phase == RELAY_DONE && rl.unconfirmed;
}

update_status_t update_split_other_info(uint8_t payload[USPLIT_INFO_PAYLOAD], uint8_t *state) {
    if (update_relay_busy()) return UPDATE_BUSY; // its requests and the other half's retry cache stay its own
    uint8_t      req[USPLIT_MSG_MAX], buf[USPLIT_MSG_MAX];
    usplit_rsp_t r;
    uint8_t      len  = usplit_req_build(req, USPLIT_OP_INFO, ++master_seq, NULL, 0);
    uint8_t      want = usplit_rsp_len(USPLIT_OP_INFO);
    for (int i = 0; i < 2; i++) {
        memset(buf, 0, sizeof(buf));
        if (update_split_port_rpc(req, len, buf, want) && usplit_rsp_parse(buf, want, USPLIT_OP_INFO, req[1], &r) == UPDATE_OK && r.status == UPDATE_OK) {
            memcpy(payload, r.payload, USPLIT_INFO_PAYLOAD);
            *state = r.state;
            return UPDATE_OK;
        }
    }
    return UPDATE_UNAVAILABLE;
}

// At most two requests without a valid answer, as INFO (each lost one costs
// the 20 ms serial timeout: R13's 50 ms per op), and a few more when the
// other half answers BUSY for a busy DMA channel (an answer within 1 ms).
#define SETTINGS_RESET_SILENT_TRIES 2
#define SETTINGS_RESET_DMA_TRIES 6

update_status_t update_split_other_settings_reset(uint8_t mode) {
    if (update_relay_busy()) return UPDATE_BUSY;
    const uint8_t        magic[5] = {'R', 'S', 'E', 'T', mode};
    const uint8_t        mlen     = mode ? 5 : 4; // mode 0: protocol 4's payload, which every protocol 4 half knows
    uint8_t              req[USPLIT_MSG_MAX], buf[USPLIT_MSG_MAX];
    usplit_rsp_t         r;
    uint8_t              want = usplit_rsp_len(USPLIT_OP_SETTINGS_RESET);
    uint8_t              len  = usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, ++master_seq, magic, mlen);
    int                  silent = 0, dma = 0, invalids = 0;
    while (silent < SETTINGS_RESET_SILENT_TRIES && dma < SETTINGS_RESET_DMA_TRIES) {
        memset(buf, 0, sizeof(buf));
        if (!update_split_port_rpc(req, len, buf, want) || usplit_rsp_parse(buf, want, USPLIT_OP_SETTINGS_RESET, req[1], &r) != UPDATE_OK) {
            silent++; // no answer, or a damaged one: the same frame again (a repeat is not run twice)
            continue;
        }
        if (r.status == UPDATE_BUSY && r.state == UPDATE_STATE_IDLE) {
            // A busy DMA channel there: nothing was written. The answer is
            // cached by seq, so a new request.
            dma++;
            len = usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, ++master_seq, magic, mlen);
            continue;
        }
        if (r.status == UPDATE_INVALID && ++invalids < RELAY_INVALID_TRIES) continue; // maybe damaged on the way
        if (r.status == UPDATE_OK || r.status == UPDATE_BUSY || r.status == UPDATE_FLASH_ERR) return (update_status_t)r.status;
        return UPDATE_UNAVAILABLE; // refused: nothing changed there
    }
    return dma >= SETTINGS_RESET_DMA_TRIES ? UPDATE_BUSY : UPDATE_UNAVAILABLE;
}

#ifdef SVAL_UPDATER_HOST_TEST
void update_split_host_reset(void) {
    memset(&sl, 0, sizeof(sl));
    memset(&hk, 0, sizeof(hk));
    memset(&rl, 0, sizeof(rl));
    master_seq     = 0;
    presence       = UPDATE_PRESENCE_NONE;
    awaiting_match = false;
    memset(&presence_last, 0, sizeof(presence_last));
}

// The other half's RAM after a reset: its session and its retry cache gone.
void update_split_host_slave_reset(void) {
    memset(&sl, 0, sizeof(sl));
    memset(&hk, 0, sizeof(hk));
}

// This half's RAM after a reset (pair: its own commit).
void update_split_host_master_reset(void) {
    memset(&rl, 0, sizeof(rl));
    presence       = UPDATE_PRESENCE_NONE;
    awaiting_match = false;
    memset(&presence_last, 0, sizeof(presence_last));
}
#else

// ---- firmware ------------------------------------------------------------------------

#    include "quantum.h"
#    include "timer.h"
#    include "split_common/transactions.h"
#    include "synchronization_util.h"
#    include "version.h" // QMK_GIT_HASH
#    include "split_pause.h"
#    include "build_reset.h"
#    include "updater.h"
#    include "hardware/sync.h"
#    include "hardware/regs/addressmap.h"
#    include "hardware/regs/dma.h"
#    include "hardware/platform_defs.h"

static update_build_id_t self_id;
static uint8_t           info_flags; // the constant bits

// P1 (keyboards/svalboard/tools/check_split_tables.py): the keyboard RPC
// entries of split_transaction_table are all zero in the ELF (they are filled
// at boot), so the order of SPLIT_TRANSACTION_IDS_KB cannot be read from it.
// This table names each ID: 'S','V','K','B', the count of entries, then per
// entry its ID and a two-letter tag. Two builds that order the IDs
// differently differ here. update_split_init() reads it, so it is linked.
const uint8_t sval_split_kb_ids[] = {'S', 'V', 'K', 'B', 3, KEYBOARD_SYNC_A, 'S', 'A', KEYBOARD_SYNC_B, 'S', 'B', KEYBOARD_UPDATE, 'U', 'P'};

static void keyboard_update_rpc(uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    update_split_slave_rpc((const uint8_t *)in, in_len, (uint8_t *)out, out_len);
}

void update_split_init(void) {
    update_device_t dev;
    update_device_self(&dev);
    self_id.fw_version    = SVAL_FW_VERSION;
    self_id.git_hash      = usplit_git_hash32(QMK_GIT_HASH, &self_id.git_dirty);
    self_id.updater_proto = UPDATE_PROTOCOL_VERSION;
    self_id.hand          = dev.hand;
    self_id.pointing_id   = dev.pointing_id;
    // update_flash_available() probes the die the first time: here, on the
    // main thread, never in the SlaveThread.
    info_flags = (update_flash_available() ? 0x01 : 0) | (dev.release_build ? 0x04 : 0) | (self_id.git_dirty ? 0x20 : 0);
#    ifdef SVAL_UPDATE_TEST_HOOKS
    info_flags |= 0x08;
#    endif
    transaction_register_rpc(KEYBOARD_UPDATE, keyboard_update_rpc);
    (void)*(const volatile uint8_t *)&sval_split_kb_ids[4]; // kept in the ELF for P1
}

void update_split_port_build_id(update_build_id_t *id) {
    *id = self_id;
}

uint8_t update_split_port_info_flags(void) {
    return info_flags | (updater_port_settings_failing() ? 0x02 : 0);
}

bool update_split_port_updating(void) {
    return updater_active();
}

uint32_t update_split_port_now_ms(void) {
    return timer_read32();
}

void update_split_port_lock(void) {
    split_shared_memory_lock();
}

void update_split_port_unlock(void) {
    split_shared_memory_unlock();
}

#    define DMA_CH_STRIDE 0x40u
_Static_assert(DMA_CH1_CTRL_TRIG_OFFSET - DMA_CH0_CTRL_TRIG_OFFSET == DMA_CH_STRIDE, "DMA channel stride");

update_status_t update_split_port_program_page(uint32_t off, const uint8_t *page) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (*(volatile uint32_t *)(DMA_BASE + DMA_CH_STRIDE * ch + DMA_CH0_CTRL_TRIG_OFFSET) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            restore_interrupts(irq);
            return UPDATE_BUSY;
        }
    }
    update_status_t st = update_flash_program_page(off, page); // its own interrupts-off section nests in this one
    restore_interrupts(irq);
    return st;
}

bool update_split_port_rpc(const uint8_t *req, uint8_t req_len, uint8_t *rsp, uint8_t rsp_len) {
    return transaction_rpc_exec(KEYBOARD_UPDATE, req_len, req, rsp_len, rsp);
}

bool update_split_port_pause(bool on) {
    return sval_split_pause(on);
}

bool update_split_port_slot_read(uint32_t off, uint8_t *dst, uint32_t n) {
    return slot_copy(off, dst, n);
}

// This half's own image, uncached, a word at a time: only inside the
// firmware area [0, SVAL_UPDATE_MAX_IMAGE).
bool update_split_port_self_read(uint32_t off, uint8_t *dst, uint32_t n) {
    if (off % 4 || n % 4 || off > SVAL_UPDATE_MAX_IMAGE || n > SVAL_UPDATE_MAX_IMAGE - off) return false;
    const volatile uint32_t *p = (const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + off);
    for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)dst)[i] = p[i];
    return true;
}

// SETTINGS_RESET on the half without USB (D42), in the SlaveThread: the same
// DMA check as a PAGE's program (R6), then one page program over the record.
update_status_t update_split_port_settings_invalidate(uint8_t mode) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (*(volatile uint32_t *)(DMA_BASE + DMA_CH_STRIDE * ch + DMA_CH0_CTRL_TRIG_OFFSET) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            restore_interrupts(irq);
            return UPDATE_BUSY;
        }
    }
    bool ok = build_reset_invalidate(mode); // its own interrupts-off section nests in this one
    restore_interrupts(irq);
    return ok ? UPDATE_OK : UPDATE_FLASH_ERR;
}

void update_split_port_reboot(void) {
    soft_reset_keyboard();
}
#endif
