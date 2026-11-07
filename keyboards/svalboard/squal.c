// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Trackball sensor samples, Sval 0x39 (squal.h, pointing_squal.h, docs/protocol.md).
#include "quantum.h"
#include "transactions.h"
#include "transport.h"
#include "split_util.h"
#include "squal.h"
#include <string.h>
#ifdef SVAL_UPDATER
#    include "split_pause.h"
#endif

static bool              capable;
static bool              is3389;
static bool              requested; // a host has asked at least once
static bool              armed;     // the ring is filling for the current host session
static uint32_t          last_req_ms;
static uint32_t          bin_start_ms;
static uint16_t          reads_per_s;
static sval_squal_acc_t  acc;
static sval_squal_ring_t ring;

void sval_squal_set_capable(bool is_pmw3389) {
    capable = true;
    is3389  = is_pmw3389;
}

bool sval_squal_wanted(void) {
    if (capable && requested && timer_elapsed32(last_req_ms) < SVAL_HOST_ACTIVE_MS) return true;
    armed = false; // the next session starts with an empty ring
    return false;
}

void sval_squal_record(const sval_squal_read_t *r) {
    uint32_t now = timer_read32();
    if (!armed) {
        sval_squal_ring_clear(&ring);
        sval_squal_acc_clear(&acc);
        acc.reads    = 0;
        bin_start_ms = now;
        armed        = true;
    }
    sval_squal_acc_add(&acc, r);
    uint32_t span = now - bin_start_ms;
    if (span < SVAL_SQUAL_BIN_MS) return;
    sval_squal_sample_t s;
    if (sval_squal_acc_finish(&acc, &s)) {
        sval_squal_ring_push(&ring, &s);
        uint32_t rate = (uint32_t)acc.reads * 1000u / span;
        reads_per_s   = rate > 0xFFFF ? 0xFFFF : (uint16_t)rate;
    }
    sval_squal_acc_clear(&acc);
    acc.reads    = 0;
    bin_start_ms = now;
}

// This half's answer; marks the host as polling. All zero (format 0) when no PMW sensor runs here.
static void local_reply(uint8_t *out, uint8_t op, uint16_t arg) {
    last_req_ms = timer_read32();
    requested   = true;
    if (!capable) {
        memset(out, 0, SVAL_SQUAL_REPLY_LEN);
        return;
    }
    uint8_t flags = SVAL_SQUAL_F_CAPABLE | (is3389 ? SVAL_SQUAL_F_3389 : 0) | (is_keyboard_left() ? 0 : SVAL_SQUAL_F_RIGHT);
    if (op == SVAL_SQUAL_OP_STATS)
        sval_squal_pack_stats(out, &ring, arg, reads_per_s, flags);
    else
        sval_squal_pack_samples(out, &ring, arg, flags);
}

bool sval_squal_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 4 || in[0] != SVAL_SQUAL_RPC_OP || out_len < SVAL_SQUAL_RPC_REPLY) return false;
    memset(out, 0, SVAL_SQUAL_RPC_REPLY);
    out[0] = SVAL_SQUAL_RPC_OP;
    local_reply(out + 1, in[1], (uint16_t)(in[2] | (in[3] << 8)));
    return true;
}

// The other half's reply to [op][arg], or false when it can't answer now.
static bool remote(uint8_t op, uint16_t arg, uint8_t *out) {
#ifdef SVAL_UPDATER
    if (sval_split_paused()) return false;
#endif
    if (!is_transport_connected()) return false;
    uint8_t req[4] = {SVAL_SQUAL_RPC_OP, op, (uint8_t)arg, (uint8_t)(arg >> 8)};
    uint8_t reply[SVAL_SQUAL_RPC_REPLY];
    if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(reply), reply) || reply[0] != SVAL_SQUAL_RPC_OP) return false;
    memcpy(out, reply + 1, SVAL_SQUAL_REPLY_LEN);
    return true;
}

// Request:  [op] [hand: 0 left, 1 right] [arg lo] [arg hi]  (after [0xDF][0x39])
//           SAMPLES arg = the cursor (the next bin wanted); STATS arg = bins in the window (0 = all)
// Response: SVAL_SQUAL_REPLY_LEN bytes from the op byte on (pointing_squal.h); format 0 = no data
void sval_squal_command(uint8_t *data, uint8_t length) {
    if (length < SVAL_SQUAL_REPLY_LEN) return;
    uint8_t  op   = data[0];
    uint8_t  hand = data[1];
    uint16_t arg  = (uint16_t)(data[2] | (data[3] << 8));
    memset(data, 0, length);
    if (!is_keyboard_master() || hand > 1 || op > SVAL_SQUAL_OP_STATS) return;
    const uint8_t mine = is_keyboard_left() ? 0 : 1;
    if (hand == mine) {
        local_reply(data, op, arg);
    } else if (!remote(op, arg, data)) {
        memset(data, 0, length);
    }
}
