// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Svalboard: which transaction an EXECUTE_RPC on the other half may run
// (transactions.c, slave_rpc_exec_callback). rpc_info comes from the wire,
// behind only a CRC-8, and its transaction_id is an int8_t: a byte of 0x80 or
// more read as signed passes "< NUM_TOTAL_TRANSACTIONS" and indexes before
// split_transaction_table. The ID is taken as the unsigned byte, and only the
// IDs transaction_rpc_exec() sends (after GET_RPC_RESP_DATA) run: never a core
// one, nor EXECUTE_RPC itself, whose callback is this one.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "transaction_id_define.h"

#if defined(SPLIT_TRANSACTION_RPC)
static inline bool split_rpc_exec_id_ok(int8_t transaction_id) {
    uint8_t id = (uint8_t)transaction_id;
    return id > GET_RPC_RESP_DATA && id < NUM_TOTAL_TRANSACTIONS;
}
#endif // defined(SPLIT_TRANSACTION_RPC)
