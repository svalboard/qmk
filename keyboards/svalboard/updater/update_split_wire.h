// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// In-firmware updater, M2: what travels over the split link. Pure functions
// over bytes, shared by the firmware (update_split.c) and the host tests
// (tests/sval_updater/test_split.c). Plan: docs/updater-plan.md, M2, D13-D17.
//
// Two messages:
//
// 1. Presence (D17), in the response to the existing KEYBOARD_SYNC_A RPC. In
//    SVAL_UPDATER builds that response (presence_rsp_t, kb/svalboard.h) is
//    the 7-byte pointer report, then these 17 bytes: update_presence_t, 24
//    bytes in all, inside the 32-byte RPC buffer. A half that does not fill
//    them leaves stale bytes, which fail the magic or the CRC.
//
// 2. KEYBOARD_UPDATE, a new split RPC (SPLIT_TRANSACTION_IDS_KB, SVAL_UPDATER
//    builds only). Master to slave, at most 32 bytes each way
//    (RPC_M2S_BUFFER_SIZE / RPC_S2M_BUFFER_SIZE, quantum/split_common/transport.h),
//    except PAGE2 requests (the fast path, below), at most 248 bytes:
//
//      request  [op][seq][payload ...][crc16 LE]
//      response [status][op][seq][state][payload ...][crc16 LE]
//
//    The RPC length is the frame length. crc16 is CRC-16/CCITT-FALSE over
//    every byte before it. seq is the master's message counter: a retry (the
//    response was lost) repeats the request byte for byte with the same seq,
//    and the slave answers it from its cache without running it again. The
//    response's payload length is fixed per op (usplit_rsp_payload_len), so
//    the master always asks for the right number of bytes back.
//
//    op          request payload                 response payload
//    0x00 INFO    -                               proto, hand, pointing_id, fw_version u32, git_hash u32,
//                                                 storage_format, security_epoch u16, flags (bit0 die is
//                                                 16 MiB, bit1 settings writes failing, bit2 release build,
//                                                 bit3 test hooks, bit4 the fast path (FAST, below),
//                                                 bit5 git tree dirty)                               (15 B)
//    0x01 BEGIN   off u8, n 1..26, bytes          bytes filled u8 (of 108)                            (1 B)
//                 The 108-byte manifest, in order; offset 0 starts a new session. The
//                 fragment that completes it starts the slave's own manifest check and
//                 the erase, in its housekeeping (async).
//    0x02 PAGE    off u24, n 1..24, bytes         next offset u24                                     (3 B)
//                 Image bytes. A fragment never crosses a 256 B page; the slave programs a
//                 page when its last fragment arrives. An earlier fragment is acknowledged and
//                 not written again (idempotent by offset).
//    0x03 STATUS  -                               last error, staged u24, sectors erased u16, sectors
//                                                 to erase u16, image CRC low 16 (once VERIFIED),
//                                                 flags (bit0 session open)                         (11 B)
//    0x04 END     -                               -   all pages sent: async image verify
//    0x05 COMMIT  image CRC low 16                -   needs VERIFIED, behind the master's pause
//    0x06 ABORT   -                               -   never erases
//    0x07 SETTINGS_RESET 'R' 'S' 'E' 'T' [mode]   -   protocol 4 (D42): only with no session (else
//                 BUSY); invalidates this half's build reset record (one page program, no
//                 erase), then this half resets itself about 300 ms later, so its next boot
//                 resets every stored setting (D39). Protocol 2 and 3 halves do not know the op
//                 and send no answer. mode (D44, optional): 0 defaults, 1 blank (then every
//                 key KC_NO); a half from before D44 answers a 5-byte payload INVALID.
//    0x08 PAGE2   off u24, n 1..240, bytes        next offset u24                                     (3 B)
//                 The fast path's PAGE: up to 240 image bytes, and a fragment may cross a
//                 256 B page (the slave programs the page it completes, then keeps the rest).
//                 The same order and repeat rules as PAGE. Up to 248 bytes on the wire.
//    0x09 FAST    mode u8 (1)                     -   the next session (BEGIN offset 0) runs the
//                 fast path: the slave erases its slot in 64 KiB blocks, hashes pages as they
//                 arrive, takes PAGE2 and stops scanning its keys while it receives. Only with
//                 no session (else BUSY).
//
//    The fast path (both halves on a build with it). An old half takes the RPC
//    length from the wire with no bound (quantum/split_common/transactions.c,
//    slave_rpc_info_callback) into a 32-byte buffer, so a frame over 32 bytes
//    must never reach one. The master reads INFO at the start of every relay,
//    and sends FAST and any PAGE2 only after INFO flags bit4 was seen in that
//    relay; after any request that went without a valid answer (the other half
//    may have restarted, perhaps into an old build), it reads INFO again before
//    the next PAGE2. It never tries a large frame to see whether it works.
//
//    status is an update_status_t, state the slave's update_state_t. A request
//    that fails its CRC or its op's length rules is answered INVALID and
//    changes nothing.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "update_manifest.h"

// ---- checksums ------------------------------------------------------------------

// CRC-8, poly 0x07, init 0x00, no reflection, no xorout (CRC-8/SMBUS).
uint8_t usplit_crc8(const uint8_t *p, size_t n);
// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
uint16_t usplit_crc16(const uint8_t *p, size_t n);

// ---- what a build is, as the other half sees it ----------------------------------

typedef struct {
    uint32_t fw_version;    // SVAL_FW_VERSION (D17); 0 until M3 sets it from the tag
    uint32_t git_hash;      // first 8 hex digits of QMK_GIT_HASH; 0 when unknown (SKIP_GIT: "NA")
    bool     git_dirty;     // QMK_GIT_HASH ends in '*'
    uint8_t  updater_proto; // UPDATE_PROTOCOL_VERSION
    uint8_t  hand;          // UPDATE_HAND_*
    uint8_t  pointing_id;   // UPDATE_POINTING_*
} update_build_id_t;

// QMK_GIT_HASH ("3b2ca7e139", "3b2ca7e139*" when dirty, "NA") as a number:
// its first 8 hex digits; 0 if it does not start with 8 hex digits.
uint32_t usplit_git_hash32(const char *s, bool *dirty);

// ---- presence (D17) --------------------------------------------------------------

#define UPDATE_PRESENCE_MAGIC 0x5650u // bytes 'P' 'V' on the wire
#define UPDATE_PRESENCE_STRUCT_VERSION 1
#define UPDATE_PRESENCE_FLAG_DIRTY 0x01   // git tree dirty
#define UPDATE_PRESENCE_FLAG_UPDATING 0x02 // the updater is active on that half

typedef struct __attribute__((packed)) {
    uint16_t magic;         // UPDATE_PRESENCE_MAGIC
    uint8_t  struct_ver;    // UPDATE_PRESENCE_STRUCT_VERSION
    uint32_t fw_version;
    uint32_t git_hash;
    uint8_t  updater_proto;
    uint8_t  hand;
    uint8_t  pointing_id;
    uint8_t  flags;         // UPDATE_PRESENCE_FLAG_*
    uint8_t  reserved;      // 0
    uint8_t  crc8;          // usplit_crc8 over the 16 bytes before it
} update_presence_t;

#define UPDATE_PRESENCE_BYTES 17 // = sizeof(presence_rsp_t.update)
_Static_assert(sizeof(update_presence_t) == UPDATE_PRESENCE_BYTES, "presence response is 17 bytes");

void usplit_presence_encode(update_presence_t *p, const update_build_id_t *self, bool updating);

// The other half's presence response against this build: UPDATE_PRESENCE_*
// (update_manifest.h), never UPDATE_PRESENCE_NONE. Mismatch rules, in order:
// fails magic, struct version or CRC (or is shorter than 17 B): INVALID;
// same hand as this half: SAME_HAND; a different fw_version, git hash or
// updater protocol: VERSION; else MATCH. pointing_id may differ (D18, P1).
uint8_t usplit_presence_compare(const void *rsp, size_t len, const update_build_id_t *self, update_presence_t *out);

// ---- KEYBOARD_UPDATE -------------------------------------------------------------

#define USPLIT_MSG_MAX 32 // RPC_M2S_BUFFER_SIZE / RPC_S2M_BUFFER_SIZE
#define USPLIT_REQ_HDR 2  // op, seq
#define USPLIT_RSP_HDR 4  // status, op, seq, state
#define USPLIT_CRC_BYTES 2
#define USPLIT_BEGIN_DATA_MAX (USPLIT_MSG_MAX - USPLIT_REQ_HDR - 2 - USPLIT_CRC_BYTES) // 26
#define USPLIT_PAGE_DATA_MAX (USPLIT_MSG_MAX - USPLIT_REQ_HDR - 4 - USPLIT_CRC_BYTES)  // 24
#define USPLIT_PAGE_BYTES 256
// The fast path: PAGE2 requests, up to USPLIT_MSG2_MAX bytes (the RPC buffer
// toward the half without USB is that large in builds with it: config.h).
#define USPLIT_PAGE2_DATA_MAX 240
#define USPLIT_MSG2_MAX (USPLIT_REQ_HDR + 4 + USPLIT_PAGE2_DATA_MAX + USPLIT_CRC_BYTES) // 248
#define USPLIT_INFO_FAST 0x10 // INFO flags bit4

typedef enum {
    USPLIT_OP_INFO   = 0x00,
    USPLIT_OP_BEGIN  = 0x01,
    USPLIT_OP_PAGE   = 0x02,
    USPLIT_OP_STATUS = 0x03,
    USPLIT_OP_END    = 0x04,
    USPLIT_OP_COMMIT = 0x05,
    USPLIT_OP_ABORT  = 0x06,
    USPLIT_OP_SETTINGS_RESET = 0x07, // protocol 4 (D42)
    USPLIT_OP_PAGE2          = 0x08, // the fast path
    USPLIT_OP_FAST           = 0x09, // the fast path
    USPLIT_OP_COUNT,
    // SVAL_UPDATE_TEST_HOOKS builds only (unknown elsewhere): the half
    // without USB resets itself 50 ms after answering, as a power cycle would
    // leave it (SVAL_RELAY_FAULTS, docs/updater.md).
    USPLIT_OP_TEST_RESET = 0x7E,
} usplit_op_t;

#define USPLIT_INFO_PAYLOAD 15
#define USPLIT_STATUS_PAYLOAD 11

// The response payload length for op, or -1 for an unknown op.
int usplit_rsp_payload_len(uint8_t op);
// The whole response frame for op (header, payload, CRC), or 0 for an unknown op.
uint8_t usplit_rsp_len(uint8_t op);

typedef struct {
    uint8_t        op, seq;
    const uint8_t *payload;
    uint8_t        payload_len;
    uint16_t       crc; // the frame's CRC (for the slave's replay check)
    // Decoded fields, by op:
    uint32_t       off;  // BEGIN (u8), PAGE (u24)
    uint8_t        n;    // BEGIN, PAGE
    const uint8_t *data; // BEGIN, PAGE: n bytes
    uint16_t       crc_lo16; // COMMIT
    uint8_t        mode;     // SETTINGS_RESET (D44): 0 defaults, 1 blank; FAST: 1
} usplit_req_t;

// Build a request frame into buf (USPLIT_MSG_MAX bytes; USPLIT_MSG2_MAX for
// PAGE2). Returns its length, or 0 if the payload breaks the op's rules.
uint8_t usplit_req_build(uint8_t *buf, uint8_t op, uint8_t seq, const uint8_t *payload, uint8_t payload_len);
uint8_t usplit_req_begin(uint8_t *buf, uint8_t seq, uint8_t off, const uint8_t *data, uint8_t n);
uint8_t usplit_req_page(uint8_t *buf, uint8_t seq, uint32_t off, const uint8_t *data, uint8_t n);
uint8_t usplit_req_commit(uint8_t *buf, uint8_t seq, uint16_t crc_lo16);
uint8_t usplit_req_page2(uint8_t *buf, uint8_t seq, uint32_t off, const uint8_t *data, uint8_t n);

// Check and decode a request frame. UPDATE_OK, or UPDATE_INVALID (length, CRC,
// unknown op, or the op's payload rules) with *r filled as far as known.
update_status_t usplit_req_parse(const uint8_t *buf, uint8_t len, usplit_req_t *r);

// Build a response for op into buf (cap bytes). payload is
// usplit_rsp_payload_len(op) bytes, or NULL for zeros. Returns the frame
// length, or 0 if op is unknown or cap is too small.
uint8_t usplit_rsp_build(uint8_t *buf, uint8_t cap, uint8_t status, uint8_t op, uint8_t seq, uint8_t state, const uint8_t *payload);

typedef struct {
    uint8_t        status, op, seq, state;
    const uint8_t *payload;
    uint8_t        payload_len;
} usplit_rsp_t;

// Master side: check a response to (op, seq). UPDATE_OK, or UPDATE_INVALID if
// its length, CRC, op or seq is wrong (a garbled or stale answer: retry).
update_status_t usplit_rsp_parse(const uint8_t *buf, uint8_t len, uint8_t op, uint8_t seq, usplit_rsp_t *r);

// ---- PAGE fragments into whole pages (slave) -------------------------------------

typedef struct {
    uint32_t image_len; // whole pages
    uint32_t next;      // image bytes received; [0, next) are in pages already programmed or in page[]
    uint8_t  page[USPLIT_PAGE_BYTES];
} usplit_pages_t;

void usplit_pages_init(usplit_pages_t *a, uint32_t image_len);

// One PAGE fragment. UPDATE_OK when taken or already held (a repeat: nothing
// is rewritten); UPDATE_OUT_OF_ORDER past the next expected offset;
// UPDATE_OVERRUN past image_len; UPDATE_INVALID if n is 0, above
// USPLIT_PAGE_DATA_MAX, or the fragment crosses a page. *page_done is the
// offset of a page this fragment completed (page[] holds it), else UINT32_MAX.
update_status_t usplit_pages_accept(usplit_pages_t *a, uint32_t off, const uint8_t *data, uint8_t n, uint32_t *page_done);

// One PAGE2 fragment (up to USPLIT_PAGE2_DATA_MAX bytes, which may cross a
// page). The bytes of the page it completes go to program(page offset, page)
// first; only when that returns UPDATE_OK is the rest kept and the fragment
// taken. Any other result is returned and nothing is taken (BUSY: send it
// again). Otherwise as usplit_pages_accept.
typedef update_status_t (*usplit_program_t)(uint32_t off, const uint8_t *page);
update_status_t usplit_pages_accept2(usplit_pages_t *a, uint32_t off, const uint8_t *data, uint8_t n, usplit_program_t program);
