#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { KEYBOARD_SYNC_B = 1 };
bool transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out);
