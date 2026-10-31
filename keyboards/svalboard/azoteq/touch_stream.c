// SPDX-License-Identifier: GPL-2.0-or-later
// The touch page of POINTING_MOTION (Sval 0x2D page 1, docs/protocol.md
// "Touchpad touch"): each pad's touch as the chip's tap and press-and-hold
// rules see it, for the editor's touchpad visualizer. Native touchpad builds
// only (SVAL_AZOTEQ_MULTITOUCH), which read finger positions from the chip.
//
// It only watches. Each half follows its own pad (pad_touch.c) while a host
// has read the page within SVAL_HOST_ACTIVE_MS; the USB half fetches the
// other half's block over the Scan Lab RPC (KEYBOARD_SYNC_B, its own op)
// every SVAL_TOUCH_FETCH_MS while that lasts, and never otherwise. Nothing
// here changes what the pads report.
#include "quantum.h"
#include "transactions.h"
#include "transport.h"
#include "split_util.h"
#include "svalboard.h"
#include "multitouch.h"
#include "pad_gestures.h"
#include "pad_touch.h"
#include <string.h>
#ifdef SVAL_UPDATER
#    include "updater/updater.h"
#endif

#define SVAL_TOUCH_RPC_OP 0xA8 // [op] -> [op][bit 0 a block][16-byte block]
#define SVAL_TOUCH_RPC_REPLY (2 + SVAL_TOUCH_BLOCK_BYTES)
#ifndef SVAL_TOUCH_FETCH_MS
#    define SVAL_TOUCH_FETCH_MS 16
#endif
#ifndef SVAL_HOST_ACTIVE_MS
#    define SVAL_HOST_ACTIVE_MS 1500
#endif
#define SVAL_TOUCH_PAGE_FORMAT 2
#define SVAL_TOUCH_PAGE_BYTES 48

void sval_pad_touch_config(sval_pad_touch_cfg_t *cfg); // azoteq.c

static sval_pad_touch_t local;
static uint32_t         wanted_until;
static bool             wanted_once;
// USB half: the other pad's block, events held until the page is read.
static uint8_t  remote[SVAL_TOUCH_BLOCK_BYTES];
static uint32_t fetched_at, remote_at, page_read_at;
static bool     page_read_once;

bool sval_pad_touch_supported(void) {
    return true;
}

static bool wanted(void) {
    return wanted_once && (int32_t)(wanted_until - timer_read32()) > 0;
}

static void want(void) {
    if (!wanted()) memset(&local, 0, sizeof(local)); // a fresh start: no touch from before
    wanted_until = timer_read32() + SVAL_HOST_ACTIVE_MS;
    wanted_once  = true;
}

// One pad read (multitouch.c): raw is the 45 bytes from register 0x000C.
void sval_touch_sample(const uint8_t *raw, bool valid) {
    if (!wanted()) return;
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    const uint16_t x = (uint16_t)raw[10] << 8 | raw[11], y = (uint16_t)raw[12] << 8 | raw[13];
    sval_pad_touch_sample(&local, &cfg, valid, raw[5], raw[1], raw[2], x, y, timer_read32());
}

// Half without USB: the USB half asks for this pad's block. Runs in the split
// transport's callback: no I2C, no waiting.
bool sval_touch_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 1 || in[0] != SVAL_TOUCH_RPC_OP || out_len < SVAL_TOUCH_RPC_REPLY) return false;
    want();
    memset(out, 0, SVAL_TOUCH_RPC_REPLY);
    out[0] = SVAL_TOUCH_RPC_OP;
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    sval_pad_touch_pack(&local, &cfg, timer_read32(), &out[2]);
    out[1] = (out[2] & SVAL_TOUCH_VALID) ? 1 : 0;
    return true;
}

// USB half, from housekeeping: the other pad, while the page is being read.
void sval_touch_task(void) {
    if (!is_keyboard_master() || !wanted()) return;
    const uint8_t other = is_keyboard_left() ? 2 : 1;
    if (!(sval_pad_hands() & other) || !is_transport_connected()) return;
#ifdef SVAL_UPDATER
    if (updater_active()) return;
#endif
    if (timer_elapsed32(fetched_at) < SVAL_TOUCH_FETCH_MS) return;
    fetched_at                             = timer_read32();
    const uint8_t req[2]                   = {SVAL_TOUCH_RPC_OP, 0};
    uint8_t       reply[SVAL_MT_RPC_REPLY] = {0};
    if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, SVAL_MT_RPC_REPLY, reply)) return;
    if (reply[0] != SVAL_TOUCH_RPC_OP || !(reply[1] & 1)) return; // older firmware answers zeros
    sval_pad_touch_merge(remote, &reply[2]);
    remote_at = timer_read32();
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

// Sval 0x2D page 1 (docs/protocol.md "Touchpad touch").
void sval_pointing_touch_kb(uint8_t *out, uint8_t length) {
    if (length < SVAL_TOUCH_PAGE_BYTES || !is_keyboard_master()) return;
    want();
    const uint8_t  s = sval_pad_gestures();
    const uint16_t t = sval_pad_tuning();
    out[0]           = SVAL_TOUCH_PAGE_FORMAT;
    out[1]           = (sval_pad_tap_on(s) ? 1 : 0) | (sval_pad_two_finger_on(s) ? 2 : 0) | (sval_pad_hold_on(s) ? 4 : 0);
    uint32_t since   = timer_elapsed32(page_read_at);
    out[2]           = !page_read_once ? 0 : since > 255 ? 255 : (uint8_t)since;
    page_read_at     = timer_read32();
    page_read_once   = true;
    put16(&out[4], sval_pad_dead_zone(t));
    put16(&out[6], sval_pad_tap_time_ms(t));
    put16(&out[8], sval_pad_hold_time_ms(s));
    put16(&out[10], SVAL_PAD_WIDTH_MM100);
    put16(&out[12], SVAL_PAD_HEIGHT_MM100);
    put16(&out[46], sval_pad_scroll_in_force_mm100());

    const bool           left  = is_keyboard_left();
    uint8_t             *mine  = &out[left ? 14 : 30];
    uint8_t             *other = &out[left ? 30 : 14];
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    sval_pad_touch_pack(&local, &cfg, timer_read32(), mine);
    if (mine[0] & SVAL_TOUCH_VALID && sval_mt_os_touchpad(left ? 0 : 1)) mine[0] |= SVAL_TOUCH_OS;
    // The other pad's last block, while it is fresh (the link answers).
    if ((remote[0] & SVAL_TOUCH_VALID) && timer_elapsed32(remote_at) < 250) {
        memcpy(other, remote, SVAL_TOUCH_BLOCK_BYTES);
        if (sval_mt_os_touchpad(left ? 1 : 0)) other[0] |= SVAL_TOUCH_OS;
    }
    remote[1] = 0; // its events are read
}
