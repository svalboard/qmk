// Host-test stand-in for QMK's transactions.h, only what squal.c uses.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define KEYBOARD_SYNC_B 0
static inline bool transaction_rpc_exec(int8_t id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    (void)id, (void)in_len, (void)in, (void)out_len, (void)out;
    return false;
}
