// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Pair UF2: the record and the boot relay (see update_pair.h and
// docs/pair-uf2.md).
//
// At boot, on the half with USB, a valid record whose manifest is the image
// this half runs (SHA-512 over flash [0, image_len)) sends that image to the
// other half with the M2 relay, and commits it there, with no chord: copying
// a UF2 in BOOTSEL is the customer's own act on their own board, and only
// that writes a record (update_pair.h). The other half checks the manifest
// and the image itself, against its own floors (D7, D19), exactly as for a
// relay a host started. Then the record is erased.
//
// When the record is not used it is erased, except when no half that can take
// the image answered this boot: then it is kept for the next boot of the half
// with USB, at most UPDATE_PAIR_BOOT_TRIES boots in all. A kept record never
// installs something stale:
//   - it must name the image this half runs (a different image here: erased);
//   - the other half's fingerprint (fw version, git hash, updater protocol)
//     is stored at its first answer, and any later boot that finds a
//     different one erases the record (the other half was flashed meanwhile);
//   - a later boot that meets the other half for the first time does not send
//     it an older release than it runs (fw version);
//   - a boot of this half without USB erases it (the cable moved).

#include <string.h>
#include "update_pair.h"
#include "update_flash.h"
#include "update_split.h"
#include "updater_port.h"
#include "sha512.h" // vendor/: SHA-512 from Monocypher

// The other half's presence must answer within this long of the start (it
// boots with this half, and pings every 500 ms).
#ifndef SVAL_UPDATE_PAIR_PEER_WAIT_MS
#    define SVAL_UPDATE_PAIR_PEER_WAIT_MS 15000
#endif
// Relays per boot, and the pause after a failed one (a link failure).
#ifndef SVAL_UPDATE_PAIR_RELAY_TRIES
#    define SVAL_UPDATE_PAIR_RELAY_TRIES 3
#endif
#ifndef SVAL_UPDATE_PAIR_RETRY_MS
#    define SVAL_UPDATE_PAIR_RETRY_MS 2000
#endif
// The red light after a record that was refused, damaged or tried too often.
#ifndef SVAL_UPDATE_PAIR_ERROR_MS
#    define SVAL_UPDATE_PAIR_ERROR_MS 30000
#endif
#define PAIR_HASH_SLICE 2048 // image bytes hashed per pass (as updater.c's VERIFY_SLICE)
#define PAIR_MIN_PEER_PROTO 3 // the relay's other end: protocol 3 and 4 halves

_Static_assert(sizeof(update_pair_state_t) <= UPDATE_PAIR_PAGE_BYTES, "the state fits page 1");

// ---- the record --------------------------------------------------------------------------

bool update_pair_record_present(const update_pair_record_t *r) {
    return r && r->magic == UPDATE_PAIR_MAGIC;
}

update_status_t update_pair_record_check(const update_pair_record_t *r, const update_device_t *dev) {
    if (!update_pair_record_present(r)) return UPDATE_INVALID;
    if (update_crc32_mpeg2((const uint8_t *)r, UPDATE_PAIR_RECORD_BYTES - 4) != r->crc) return UPDATE_INVALID;
    if (r->version != UPDATE_PAIR_RECORD_VERSION) return UPDATE_UNSUPPORTED;
    if (r->length != UPDATE_PAIR_RECORD_BYTES || r->kind != UPDATE_PAIR_KIND_RELAY_SELF || r->image_offset != 0) return UPDATE_INVALID;
    if (r->reserved[0] || r->reserved[1] || r->reserved[2]) return UPDATE_INVALID;
    sval_update_manifest_t m;
    memcpy(&m, r->manifest, sizeof(m));
    // One image for both halves only: an image built for one side, sent to
    // the other half, would leave both halves on the same side.
    if (m.hand != UPDATE_HAND_EITHER) return UPDATE_INVALID;
    return update_manifest_check(&m, dev);
}

void update_pair_record_make(update_pair_record_t *r, const uint8_t manifest[UPDATE_MANIFEST_BYTES]) {
    memset(r, 0, sizeof(*r));
    r->magic   = UPDATE_PAIR_MAGIC;
    r->version = UPDATE_PAIR_RECORD_VERSION;
    r->length  = UPDATE_PAIR_RECORD_BYTES;
    r->kind    = UPDATE_PAIR_KIND_RELAY_SELF;
    memcpy(r->manifest, manifest, UPDATE_MANIFEST_BYTES);
    r->crc = update_crc32_mpeg2((const uint8_t *)r, UPDATE_PAIR_RECORD_BYTES - 4);
}

// ---- the boot relay ----------------------------------------------------------------------

static struct {
    uint8_t                phase;   // pair_boot_phase_t
    uint8_t                outcome; // pair_boot_outcome_t
    update_status_t        error;
    bool                   retry_boot; // an earlier boot already tried this record
    uint8_t                relays;     // relays started this boot
    uint32_t               until_ms;   // WAIT: the presence deadline; RETRY, ERROR: the end
    uint32_t               off, crc_body;
    sval_update_manifest_t m;
    uint8_t                manifest[UPDATE_MANIFEST_BYTES] __attribute__((aligned(4)));
    crypto_sha512_ctx      sha;
} pb;

static void read_bytes(uint32_t off, void *dst, uint32_t n) {
    const volatile uint8_t *p = update_pair_sector_read() + off;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = p[i];
}

static void finish(uint8_t outcome, update_status_t error, bool erase, bool light) {
    pb.outcome = outcome;
    pb.error   = error;
    if (erase && update_pair_sector_erase() != UPDATE_OK) light = true; // the next boot sees it again
    if (light) {
        pb.phase    = PAIR_BOOT_ERROR;
        pb.until_ms = updater_port_now_ms() + SVAL_UPDATE_PAIR_ERROR_MS;
    } else {
        pb.phase = PAIR_BOOT_IDLE;
    }
}

// Clears bytes of the state page: the bytes of `state` that are not 0xFF.
static bool state_program(const update_pair_state_t *st) {
    uint8_t page[UPDATE_PAIR_PAGE_BYTES];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, st, sizeof(*st));
    return update_pair_state_program(page) == UPDATE_OK;
}

static void start(void) {
    pb.phase = PAIR_BOOT_IDLE;
    if (!update_flash_available()) return; // D26: no updater on another die; the record stays inert
    update_pair_record_t r;
    read_bytes(0, &r, sizeof(r));
    if (!update_pair_record_present(&r)) return; // the usual boot: one page read, nothing written
    if (!updater_port_is_master()) {
        finish(PAIR_OUT_ABANDONED, UPDATE_OK, true, false); // booted without USB: the cable moved
        return;
    }
    update_device_t dev;
    update_device_self(&dev);
    update_status_t st = update_pair_record_check(&r, &dev);
    if (st != UPDATE_OK) {
        finish(PAIR_OUT_REFUSED, st, true, true);
        return;
    }
    update_pair_state_t s;
    read_bytes(UPDATE_PAIR_STATE_OFFSET, &s, sizeof(s));
    uint8_t i = 0;
    while (i < UPDATE_PAIR_BOOT_TRIES && s.tries[i] != 0xFF) i++;
    if (i == UPDATE_PAIR_BOOT_TRIES) {
        finish(PAIR_OUT_EXHAUSTED, UPDATE_TIMEOUT, true, true);
        return;
    }
    // Counted before anything else, so a boot that never gets further still counts.
    update_pair_state_t mark;
    memset(&mark, 0xFF, sizeof(mark));
    mark.tries[i] = 0x00;
    if (!state_program(&mark)) {
        finish(PAIR_OUT_REFUSED, UPDATE_FLASH_ERR, true, true);
        return;
    }
    pb.retry_boot = i > 0;
    memcpy(pb.manifest, r.manifest, UPDATE_MANIFEST_BYTES);
    memcpy(&pb.m, r.manifest, sizeof(pb.m));
    crypto_sha512_init(&pb.sha);
    pb.off      = 0;
    pb.crc_body = UPDATE_CRC32_INIT;
    pb.phase    = PAIR_BOOT_HASH;
}

// SHA-512 and the body CRC of this half's own image, a slice per pass.
static void hash_step(void) {
    uint8_t  buf[UPDATE_PAIR_PAGE_BYTES] __attribute__((aligned(4)));
    uint32_t len = pb.m.image_len;
    uint32_t end = len - pb.off > PAIR_HASH_SLICE ? pb.off + PAIR_HASH_SLICE : len;
    while (pb.off < end) {
        uint32_t n = end - pb.off < sizeof(buf) ? end - pb.off : sizeof(buf);
        if (!update_split_port_self_read(pb.off, buf, n)) {
            finish(PAIR_OUT_REFUSED, UPDATE_FLASH_ERR, true, true);
            return;
        }
        crypto_sha512_update(&pb.sha, buf, n);
        uint32_t skip = pb.off < UPDATE_VECTORS_OFFSET ? UPDATE_VECTORS_OFFSET - pb.off : 0;
        if (skip < n) pb.crc_body = update_crc32_mpeg2_update(pb.crc_body, buf + skip, n - skip);
        pb.off += n;
    }
    if (pb.off < len) return;
    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512_final(&pb.sha, hash);
    if (memcmp(hash, pb.m.sha512, UPDATE_SHA512_BYTES) != 0) {
        // Not the image this half runs: an interrupted copy, or the record of
        // an earlier file after another install. Never relayed.
        finish(PAIR_OUT_STALE, UPDATE_BAD_HASH, true, false);
        return;
    }
    pb.phase    = PAIR_BOOT_WAIT;
    pb.until_ms = updater_port_now_ms() + SVAL_UPDATE_PAIR_PEER_WAIT_MS;
}

static bool due(uint32_t at) {
    return (int32_t)(updater_port_now_ms() - at) >= 0;
}

bool update_pair_peer_takes(void) {
    update_presence_t p;
    uint8_t           pr = update_split_other(&p);
    return (pr == UPDATE_PRESENCE_MATCH || pr == UPDATE_PRESENCE_VERSION || pr == UPDATE_PRESENCE_SAME_HAND) && p.updater_proto >= PAIR_MIN_PEER_PROTO;
}

static void wait_step(void) {
    update_presence_t p;
    uint8_t           pr = update_split_other(&p);
    bool              answers = pr == UPDATE_PRESENCE_MATCH || pr == UPDATE_PRESENCE_VERSION || pr == UPDATE_PRESENCE_SAME_HAND;
    if (!update_pair_peer_takes()) {
        // Nothing yet, or a half without the updater (or on protocol 2):
        // kept for the next boot, which may find it (or a half that takes it).
        if (due(pb.until_ms)) finish(PAIR_OUT_KEPT, answers || pr == UPDATE_PRESENCE_INVALID ? UPDATE_UNSUPPORTED : UPDATE_UNAVAILABLE, false, false);
        return;
    }
    if (update_relay_busy() || update_split_slave_active()) return; // cannot happen: host sessions get BUSY meanwhile
    update_pair_state_t s;
    read_bytes(UPDATE_PAIR_STATE_OFFSET, &s, sizeof(s));
    if (s.seen == 0x00) {
        if (s.peer_fw_version != p.fw_version || s.peer_git_hash != p.git_hash || s.peer_proto != p.updater_proto) {
            finish(PAIR_OUT_STALE, UPDATE_OK, true, false); // the other half was flashed since this record first met it
            return;
        }
    } else {
        // The first answer: a later boot met a half it never saw, which must
        // not be sent an older release than it runs.
        if (pb.retry_boot && p.fw_version > pb.m.fw_version) {
            finish(PAIR_OUT_STALE, UPDATE_EPOCH, true, false);
            return;
        }
        update_pair_state_t fp;
        memset(&fp, 0xFF, sizeof(fp));
        fp.seen            = 0x00;
        fp.peer_fw_version = p.fw_version;
        fp.peer_git_hash   = p.git_hash;
        fp.peer_proto      = p.updater_proto;
        if (!state_program(&fp)) {
            finish(PAIR_OUT_REFUSED, UPDATE_FLASH_ERR, true, true);
            return;
        }
    }
    if (!update_relay_start_self(pb.manifest, pb.m.image_len, pb.crc_body)) return; // the link cannot pause yet: next pass
    pb.relays++;
    pb.phase = PAIR_BOOT_RELAY;
}

static void relay_step(void) {
    update_relay_info_t ri;
    update_relay_info(&ri);
    switch (ri.phase) {
        case RELAY_VERIFIED:
            update_relay_commit(); // no chord: the copy in BOOTSEL was the consent; retried next pass if the link is busy
            return;
        case RELAY_DONE:
            finish(PAIR_OUT_DONE, UPDATE_OK, true, false);
            return;
        case RELAY_FAILED:
            if (update_relay_busy()) return; // still telling the other half
            if (ri.error == UPDATE_UNAVAILABLE || ri.error == UPDATE_TIMEOUT) {
                // The link failed (or the other half went silent): again,
                // a few times; then kept for the next boot.
                if (pb.relays < SVAL_UPDATE_PAIR_RELAY_TRIES) {
                    pb.phase    = PAIR_BOOT_RETRY;
                    pb.until_ms = updater_port_now_ms() + SVAL_UPDATE_PAIR_RETRY_MS;
                } else {
                    finish(PAIR_OUT_KEPT, ri.error, false, false);
                }
                return;
            }
            finish(PAIR_OUT_REFUSED, ri.error, true, true); // the other half refused it (its floors, the image)
            return;
        default:
            return;
    }
}

void update_pair_task(void) {
    switch (pb.phase) {
        case PAIR_BOOT_INIT:
            start();
            break;
        case PAIR_BOOT_HASH:
            hash_step();
            break;
        case PAIR_BOOT_WAIT:
            wait_step();
            break;
        case PAIR_BOOT_RELAY:
            relay_step();
            break;
        case PAIR_BOOT_RETRY:
            if (due(pb.until_ms)) {
                pb.phase    = PAIR_BOOT_WAIT;
                pb.until_ms = updater_port_now_ms() + SVAL_UPDATE_PAIR_PEER_WAIT_MS;
            }
            break;
        case PAIR_BOOT_ERROR:
            if (due(pb.until_ms)) pb.phase = PAIR_BOOT_IDLE;
            break;
        default:
            break;
    }
}

bool update_pair_busy(void) {
    switch (pb.phase) {
        case PAIR_BOOT_HASH:
        case PAIR_BOOT_WAIT:
        case PAIR_BOOT_RELAY:
        case PAIR_BOOT_RETRY:
            return true;
        case PAIR_BOOT_INIT: {
            // A host request before the first pass: busy if a record waits.
            update_pair_record_t r;
            if (!update_flash_available()) return false;
            read_bytes(0, &r, sizeof(r.magic));
            return update_pair_record_present(&r);
        }
        default:
            return false;
    }
}

uint8_t update_pair_phase(void) {
    return pb.phase;
}

uint8_t update_pair_outcome(void) {
    return pb.outcome;
}

update_status_t update_pair_error(void) {
    return pb.error;
}

#ifdef SVAL_UPDATER_HOST_TEST
void update_pair_host_reset(void) {
    memset(&pb, 0, sizeof(pb));
}
#endif
