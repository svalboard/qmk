// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Flicker latch, Sval 0x3D (flicker_latch.h, docs/protocol.md "Flicker latch").
#include "quantum.h"
#include "transactions.h"
#include "transport.h"
#include "split_util.h"
#include "flicker_latch.h"
#include <string.h>
#include "updater/updater_api.h"

#define HALF_ROWS (MATRIX_ROWS / 2)
_Static_assert(MATRIX_COLS <= 8, "one byte per row");
_Static_assert(1 + HALF_ROWS * MATRIX_COLS <= SVAL_FLICKER_RPC_REPLY, "counts fit the split RPC reply");

static matrix_row_t prev[HALF_ROWS], raw_now[HALF_ROWS], pressed_since[HALF_ROWS], released_since[HALF_ROWS];
static uint8_t      changes[HALF_ROWS][MATRIX_COLS];
static bool         seen_frame;

void sval_flicker_frame(const matrix_row_t *raw, uint8_t rows) {
    if (rows > HALF_ROWS) rows = HALF_ROWS;
    for (uint8_t r = 0; r < rows; r++) {
        matrix_row_t now = raw[r];
        matrix_row_t was = seen_frame ? prev[r] : now;
        matrix_row_t diff = now ^ was;
        pressed_since[r] |= now & ~was;
        released_since[r] |= ~now & was;
        for (uint8_t c = 0; diff && c < MATRIX_COLS; c++) {
            if ((diff >> c) & 1) {
                if (changes[r][c] != UINT8_MAX) changes[r][c]++;
            }
        }
        prev[r] = raw_now[r] = now;
    }
    seen_frame = true;
}

// This half's raw rows and latches into raw/pressed/released (HALF_ROWS each), then clear.
static void take_latch(uint8_t *raw, uint8_t *pressed, uint8_t *released) {
    for (uint8_t r = 0; r < HALF_ROWS; r++) {
        raw[r]      = raw_now[r];
        pressed[r]  = pressed_since[r];
        released[r] = released_since[r];
    }
    memset(pressed_since, 0, sizeof(pressed_since));
    memset(released_since, 0, sizeof(released_since));
}

static void take_counts(uint8_t *out) {
    memcpy(out, changes, sizeof(changes));
    memset(changes, 0, sizeof(changes));
}

bool sval_flicker_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 2 || in[0] != SVAL_FLICKER_RPC_OP || out_len < SVAL_FLICKER_RPC_REPLY) return false;
    memset(out, 0, SVAL_FLICKER_RPC_REPLY);
    out[0] = SVAL_FLICKER_RPC_OP;
    if (in[1] == SVAL_FLICKER_READ) {
        take_latch(out + 1, out + 1 + HALF_ROWS, out + 1 + 2 * HALF_ROWS);
    } else {
        take_counts(out + 1);
    }
    return true;
}

// The other half's reply to [op][what], or false when it can't answer now.
static bool remote(uint8_t what, uint8_t *reply) {
    if (updater_split_paused()) return false;
    if (!is_transport_connected()) return false;
    uint8_t req[2] = {SVAL_FLICKER_RPC_OP, what};
    return transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, SVAL_FLICKER_RPC_REPLY, reply) && reply[0] == SVAL_FLICKER_RPC_OP;
}

// Request:  [op] [arg]  (after [0xDF][0x3D])
// READ:   -> [op] [status] [flags: bit 0 left block valid, bit 1 right] [rows]
//            [raw: rows bytes] [pressed since: rows] [released since: rows]
// COUNTS: -> [op] [status] [flags: bit 0 valid] [half] [changes: half rows x cols]
void sval_flicker_command(uint8_t *data, uint8_t length) {
    const uint8_t need = 4 + 3 * MATRIX_ROWS;
    if (length < need || length < 4 + HALF_ROWS * MATRIX_COLS) {
        if (length > 1) data[1] = 3;
        return;
    }
    uint8_t op = data[0], arg = data[1];
    memset(data + 1, 0, length - 1);
    if (!is_keyboard_master()) {
        data[1] = 2;
        return;
    }
    const uint8_t mine = is_keyboard_left() ? 0 : 1; // this half's row block
    uint8_t       reply[SVAL_FLICKER_RPC_REPLY];
    if (op == SVAL_FLICKER_READ) {
        uint8_t *raw = data + 4, *pressed = raw + MATRIX_ROWS, *released = pressed + MATRIX_ROWS;
        data[3] = MATRIX_ROWS;
        take_latch(raw + mine * HALF_ROWS, pressed + mine * HALF_ROWS, released + mine * HALF_ROWS);
        data[2] |= 1 << mine;
        const uint8_t other = mine ^ 1;
        if (remote(SVAL_FLICKER_READ, reply)) {
            memcpy(raw + other * HALF_ROWS, reply + 1, HALF_ROWS);
            memcpy(pressed + other * HALF_ROWS, reply + 1 + HALF_ROWS, HALF_ROWS);
            memcpy(released + other * HALF_ROWS, reply + 1 + 2 * HALF_ROWS, HALF_ROWS);
            data[2] |= 1 << other;
        }
    } else if (op == SVAL_FLICKER_COUNTS && arg <= 1) {
        data[3] = arg;
        if (arg == mine) {
            take_counts(data + 4);
            data[2] = 1;
        } else if (remote(SVAL_FLICKER_COUNTS, reply)) {
            memcpy(data + 4, reply + 1, HALF_ROWS * MATRIX_COLS);
            data[2] = 1;
        }
    } else {
        data[1] = 1;
    }
    data[0] = op;
}
