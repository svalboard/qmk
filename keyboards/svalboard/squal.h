// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Trackball sensor samples (Sval 0x39, docs/protocol.md "Pointing sensor").
//
// A PMW3360/PMW3389 half reads SQUAL, Raw_Data_Sum, Maximum and Minimum_Raw_data
// and the shutter in the same SPI burst as its motion, but only while a host is
// polling 0x39 (six more bytes, about 24 us at 2 MHz). Reads are folded into
// SVAL_SQUAL_BIN_MS bins in a ring on that half. Nothing here changes the pointer
// report or its timing; with no host polling the burst is the usual six bytes.
//
// The half with USB answers for both hands: the other half's ring comes over the
// split link (KEYBOARD_SYNC_B, first byte SVAL_SQUAL_RPC_OP) only when asked.

#include <stdbool.h>
#include <stdint.h>
#include "pointing_squal.h"

#define SVAL_SQUAL_RPC_OP    0xB9
#define SVAL_SQUAL_RPC_REPLY 32

// The trackball driver started on this half.
void sval_squal_set_capable(bool is_pmw3389);
// A host polled 0x39 within SVAL_HOST_ACTIVE_MS: the burst reads the extra bytes.
bool sval_squal_wanted(void);
// One burst read (trackball.c).
void sval_squal_record(const sval_squal_read_t *r);
// Sval 0x39: data after [0xDF][0x39], answered in place.
void sval_squal_command(uint8_t *data, uint8_t length);
// KEYBOARD_SYNC_B on the half without USB: true if the request was ours.
bool sval_squal_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
