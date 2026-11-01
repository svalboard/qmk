// SPDX-License-Identifier: GPL-2.0-or-later
// The Azoteq pads' contact path for the native touchpad (SVAL_NATIVE_PTP).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Split requests, carried on the Scan Lab RPC (KEYBOARD_SYNC_B). Scan Lab's
// own ops never use these values, and firmware without them answers zeros.
// The timing counters use 0xAE the same way (perf.h).
#define SVAL_MT_RPC_SNAPSHOT 0xA7 // [op][bit 0: hints ok] -> the other pad's contacts
#define SVAL_MT_RPC_REPLY 32      // reply bytes: fits RPC_S2M_BUFFER_SIZE's default

void    sval_mt_init(void);
void    sval_mt_task(void);
void    sval_mt_sample(const uint8_t *raw, bool valid); // azoteq_iqs5xx.c: one pad read
bool    sval_mt_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
void    sval_mt_command(uint8_t *data, uint8_t length); // raw HID 0xFC op 0x42 (SVAL_TOUCH_DEV)
uint8_t sval_mt_split_hint(void);                       // half without USB: its pad's news counter, or 0
void    sval_mt_remote_hint(uint8_t hint);              // USB half: the other half's counter
bool    sval_mt_suppress_hand(uint8_t pad);             // this hand's pad mouse must be zero
bool    sval_mt_os_touchpad(uint8_t pad);               // the OS drives this hand's pad
bool    sval_mt_palm(void);                             // this half's pad has a palm down (palm rejection on): its mouse output stops

// The touch page of 0x2D (touch_stream.c).
void sval_touch_sample(const uint8_t *raw, bool valid); // one pad read, as sval_mt_sample
bool sval_touch_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
void sval_touch_task(void); // USB half, housekeeping

// svalboard.c: hands whose pointing module is an Azoteq pad (bit 0 left, bit 1 right).
uint8_t sval_pad_hands(void);
