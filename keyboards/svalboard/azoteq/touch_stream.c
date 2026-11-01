// SPDX-License-Identifier: GPL-2.0-or-later
// The touch page of POINTING_MOTION (Sval 0x2D page 1, docs/protocol.md
// "Touchpad touch"): each pad's touch as the chip's tap and press-and-hold
// rules see it, for the editor's touchpad visualizer. Native touchpad builds
// only (SVAL_AZOTEQ_MULTITOUCH), which read finger positions from the chip.
//
// And the fingers page (page 2, "Touchpad fingers"): every finger the chip
// reports on each pad, and its two-finger rule, from the same pad read.
//
// It only watches. Each half follows its own pad (pad_touch.c) while a host
// has read a page within SVAL_HOST_ACTIVE_MS, each page on its own; the USB half fetches the
// other half's block over the Scan Lab RPC (KEYBOARD_SYNC_B, its own op)
// every SVAL_TOUCH_FETCH_MS while that lasts, and never otherwise. Nothing
// here changes what the pads report.
#include "quantum.h"
#include "hal.h"
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

#define SVAL_TOUCH_RPC_OP 0xA8                                                    // [op] -> [op][bit 0 a block][16-byte block]
#define SVAL_TOUCH_RPC_REPLY (2 + SVAL_TOUCH_BLOCK_BYTES + SVAL_TOUCH_SIZE_BYTES) // [op][bit 0 a block][block][size]
#ifndef SVAL_TOUCH_FETCH_MS
#    define SVAL_TOUCH_FETCH_MS 16
#endif
#ifndef SVAL_HOST_ACTIVE_MS
#    define SVAL_HOST_ACTIVE_MS 1500
#endif
#define SVAL_FINGERS_RPC_OP 0xA9                                // [op] -> [op][bit 0 a block][27-byte block]
#define SVAL_FINGERS_RPC_REPLY (2 + SVAL_FINGERS_BLOCK_BYTES) // 29: fits the 32-byte reply
_Static_assert(SVAL_TOUCH_RPC_REPLY <= SVAL_MT_RPC_REPLY && SVAL_FINGERS_RPC_REPLY <= SVAL_MT_RPC_REPLY, "the touch replies fit the split RPC");
#define SVAL_FINGERS_PAGE_FORMAT 1
#define SVAL_FINGERS_PAGE_BYTES 56 // 2 + 2 x 27: all a wrapped Sval reply holds after [0xDF] [0x2D]
#define SVAL_TOUCH_PAGE_FORMAT 2
#define SVAL_TOUCH_PAGE_BYTES 56
// The TPS43's channel grid (IQS572: 2048 x 1792 at 256 pixels a channel), for
// the area: each channel is about 5.4 x 5.7 mm.
#define SVAL_TOUCH_CHANNELS_X 8
#define SVAL_TOUCH_CHANNELS_Y 7

void sval_pad_touch_config(sval_pad_touch_cfg_t *cfg); // azoteq.c

static sval_pad_touch_t local;
static uint32_t         wanted_until;
static bool             wanted_once;
// USB half: the other pad's block, events held until the page is read.
static uint8_t  remote[SVAL_TOUCH_BLOCK_BYTES], remote_size[SVAL_TOUCH_SIZE_BYTES];
static uint32_t fetched_at, remote_at, page_read_at;
static bool     page_read_once;
// The fingers page: this pad, and the USB half's copy of the other one.
static sval_pad_fingers_t fingers;
static uint32_t           fingers_until, fingers_fetched_at, remote_fingers_at;
static bool               fingers_once;
static uint8_t            remote_fingers[SVAL_FINGERS_BLOCK_BYTES];
// Half without USB: the blocks the split callback answers with, packed in the
// main loop after each pad read and copied under osalSysLock (as multitouch.c
// does its snapshot), so a reply never races the pad read. Events stay in
// them until a reply carries them. A fresh start (the first request after a
// pause) is applied in the main loop, never from the callback.
static uint8_t       touch_snap[SVAL_TOUCH_BLOCK_BYTES], size_snap[SVAL_TOUCH_SIZE_BYTES], fingers_snap[SVAL_FINGERS_BLOCK_BYTES];
static volatile bool fresh_page, fresh_fingers;

bool sval_pad_touch_supported(void) {
    return true;
}

static bool wanted(void) {
    return wanted_once && (int32_t)(wanted_until - timer_read32()) > 0;
}

// A fresh start (no touch from before) is applied by the main loop
// (apply_fresh), so this is safe from the split callback too.
static void want(void) {
    if (!wanted()) fresh_page = true;
    wanted_until = timer_read32() + SVAL_HOST_ACTIVE_MS;
    wanted_once  = true;
}

static bool fingers_wanted(void) {
    return fingers_once && (int32_t)(fingers_until - timer_read32()) > 0;
}

static void want_fingers(void) {
    if (!fingers_wanted()) fresh_fingers = true;
    fingers_until = timer_read32() + SVAL_HOST_ACTIVE_MS;
    fingers_once  = true;
}

// Main loop only.
static void apply_fresh(void) {
    if (fresh_page) {
        fresh_page = false;
        memset(&local, 0, sizeof(local));
        osalSysLock();
        memset(touch_snap, 0, sizeof(touch_snap));
        memset(size_snap, 0, sizeof(size_snap));
        osalSysUnlock();
    }
    if (fresh_fingers) {
        fresh_fingers = false;
        memset(&fingers, 0, sizeof(fingers));
        osalSysLock();
        memset(fingers_snap, 0, sizeof(fingers_snap));
        osalSysUnlock();
    }
}

// One pad read (multitouch.c): raw is the 45 bytes from register 0x000C.
void sval_touch_sample(const uint8_t *raw, bool valid) {
    apply_fresh();
    const bool page = wanted(), all = fingers_wanted();
    if (!page && !all) return;
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    const uint32_t now    = timer_read32();
    const bool     master = is_keyboard_master();
    // Every finger slot is in this same read: no I2C of its own.
    if (all) {
        sval_pad_fingers_sample(&fingers, &cfg, valid, raw, now);
        if (!master) {
            uint8_t blk[SVAL_FINGERS_BLOCK_BYTES];
            sval_pad_fingers_pack(&fingers, &cfg, now, blk);
            osalSysLock();
            sval_pad_fingers_merge(fingers_snap, blk);
            osalSysUnlock();
        }
    }
    if (!page) return;
    const uint16_t x = (uint16_t)raw[10] << 8 | raw[11], y = (uint16_t)raw[12] << 8 | raw[13];
    sval_pad_touch_sample(&local, &cfg, valid, raw[5], raw[1], raw[2], x, y, now);
    // The first finger's strength and area come in the same read: no I2C of its own.
    if (valid && raw[5] <= 5) sval_pad_touch_size(&local, raw[5], (uint16_t)raw[14] << 8 | raw[15], raw[16]);
    if (!master) {
        uint8_t blk[SVAL_TOUCH_BLOCK_BYTES], size[SVAL_TOUCH_SIZE_BYTES];
        sval_pad_touch_pack(&local, &cfg, now, blk);
        sval_pad_touch_pack_size(&local, size);
        osalSysLock();
        sval_pad_touch_merge(touch_snap, blk);
        memcpy(size_snap, size, sizeof(size_snap));
        osalSysUnlock();
    }
}

// Half without USB: the USB half asks for this pad's block. Runs in the split
// transport's callback: no I2C, no waiting, and no touching the pad state:
// it copies the snapshot the main loop packed (and takes its events).
bool sval_touch_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len >= 1 && in[0] == SVAL_FINGERS_RPC_OP && out_len >= SVAL_FINGERS_RPC_REPLY) {
        want_fingers();
        memset(out, 0, SVAL_FINGERS_RPC_REPLY);
        out[0] = SVAL_FINGERS_RPC_OP;
        osalSysLock();
        sval_pad_fingers_take(fingers_snap, &out[2]);
        osalSysUnlock();
        out[1] = (out[2] & SVAL_FINGERS_VALID) ? 1 : 0;
        return true;
    }
    if (in_len < 1 || in[0] != SVAL_TOUCH_RPC_OP || out_len < SVAL_TOUCH_RPC_REPLY) return false;
    want();
    memset(out, 0, SVAL_TOUCH_RPC_REPLY);
    out[0] = SVAL_TOUCH_RPC_OP;
    osalSysLock();
    sval_pad_touch_take(touch_snap, &out[2]);
    memcpy(&out[2 + SVAL_TOUCH_BLOCK_BYTES], size_snap, SVAL_TOUCH_SIZE_BYTES);
    osalSysUnlock();
    out[1] = (out[2] & SVAL_TOUCH_VALID) ? 1 : 0;
    return true;
}

// USB half: the other pad's fingers, while the fingers page is being read.
static void fetch_fingers(void) {
    if (timer_elapsed32(fingers_fetched_at) < SVAL_TOUCH_FETCH_MS) return;
    fingers_fetched_at                     = timer_read32();
    const uint8_t req[2]                   = {SVAL_FINGERS_RPC_OP, 0};
    uint8_t       reply[SVAL_MT_RPC_REPLY] = {0};
    if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, SVAL_MT_RPC_REPLY, reply)) return;
    if (reply[0] != SVAL_FINGERS_RPC_OP || !(reply[1] & 1)) return; // older firmware answers zeros
    sval_pad_fingers_merge(remote_fingers, &reply[2]);
    remote_fingers_at = timer_read32();
}

// USB half, from housekeeping: the other pad, while a page is being read.
void sval_touch_task(void) {
    if (!is_keyboard_master() || !(wanted() || fingers_wanted())) return;
    const uint8_t other = is_keyboard_left() ? 2 : 1;
    if (!(sval_pad_hands() & other) || !is_transport_connected()) return;
#ifdef SVAL_UPDATER
    if (updater_active()) return;
#endif
    if (fingers_wanted()) fetch_fingers();
    if (!wanted() || timer_elapsed32(fetched_at) < SVAL_TOUCH_FETCH_MS) return;
    fetched_at                             = timer_read32();
    const uint8_t req[2]                   = {SVAL_TOUCH_RPC_OP, 0};
    uint8_t       reply[SVAL_MT_RPC_REPLY] = {0};
    if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, SVAL_MT_RPC_REPLY, reply)) return;
    if (reply[0] != SVAL_TOUCH_RPC_OP || !(reply[1] & 1)) return; // older firmware answers zeros
    sval_pad_touch_merge(remote, &reply[2]);
    memcpy(remote_size, &reply[2 + SVAL_TOUCH_BLOCK_BYTES], SVAL_TOUCH_SIZE_BYTES); // older firmware with the page leaves it zero
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
    apply_fresh(); // a raw HID request runs in the main loop: start fresh before packing
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
    // The first finger's size per pad (bytes 48-53), and the channel grid its area counts in.
    out[3]  = 1; // bit 0: bytes 48-55 hold the finger sizes
    out[54] = SVAL_TOUCH_CHANNELS_X;
    out[55] = SVAL_TOUCH_CHANNELS_Y;

    const bool           left  = is_keyboard_left();
    uint8_t             *mine  = &out[left ? 14 : 30];
    uint8_t             *other = &out[left ? 30 : 14];
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    sval_pad_touch_pack(&local, &cfg, timer_read32(), mine);
    sval_pad_touch_pack_size(&local, &out[left ? 48 : 51]);
    if (mine[0] & SVAL_TOUCH_VALID && sval_mt_os_touchpad(left ? 0 : 1)) mine[0] |= SVAL_TOUCH_OS;
    // The other pad's last block, while it is fresh (the link answers).
    if ((remote[0] & SVAL_TOUCH_VALID) && timer_elapsed32(remote_at) < 250) {
        memcpy(other, remote, SVAL_TOUCH_BLOCK_BYTES);
        memcpy(&out[left ? 51 : 48], remote_size, SVAL_TOUCH_SIZE_BYTES);
        if (sval_mt_os_touchpad(left ? 1 : 0)) other[0] |= SVAL_TOUCH_OS;
    }
    remote[1] = 0; // its events are read
}

// Sval 0x2D page 2 (docs/protocol.md "Touchpad fingers").
void sval_pointing_fingers_kb(uint8_t *out, uint8_t length) {
    if (length < SVAL_FINGERS_PAGE_BYTES || !is_keyboard_master()) return;
    want_fingers();
    apply_fresh();
    out[0] = SVAL_FINGERS_PAGE_FORMAT;
    out[1] = sval_pad_palm();
    const bool           left  = is_keyboard_left();
    uint8_t             *mine  = &out[left ? 2 : 2 + SVAL_FINGERS_BLOCK_BYTES];
    uint8_t             *other = &out[left ? 2 + SVAL_FINGERS_BLOCK_BYTES : 2];
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    sval_pad_fingers_pack(&fingers, &cfg, timer_read32(), mine);
    if ((mine[0] & SVAL_FINGERS_VALID) && sval_mt_os_touchpad(left ? 0 : 1)) mine[0] |= SVAL_FINGERS_OS;
    if ((remote_fingers[0] & SVAL_FINGERS_VALID) && timer_elapsed32(remote_fingers_at) < 250) {
        memcpy(other, remote_fingers, SVAL_FINGERS_BLOCK_BYTES);
        if (sval_mt_os_touchpad(left ? 1 : 0)) other[0] |= SVAL_FINGERS_OS;
    }
    remote_fingers[0] &= (uint8_t)~SVAL_FINGERS_TWO_TAP; // its event is read
}
