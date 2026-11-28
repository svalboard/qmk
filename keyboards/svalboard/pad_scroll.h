// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad's two-finger scroll as wheel counts (keymaps/keymap_support.c).
//
// The Azoteq chip reports a two-finger scroll as pixel motion, at the pad's
// full resolution (2048 x 1792 over 43 x 40 mm: 47.6 pixels per mm across,
// 44.8 up and down). The wheel is declared at 120 counts per detent (config.h,
// POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER). Windows applies that; macOS treats
// each count as a line, so on a Mac the counts are divided by the pad's own
// divider (Drag Scroll's MAC_DIVISOR and Velocity Scroll stay as shipped).
//
// The distance is fixed in pad pixels per detent, so the pointer's DPI does not
// change it; the hand's Scroll speed (scroll_speed.h, a percent) multiplies it,
// before the Mac divider, so 2.0x is twice as fast on both. Remainders carry,
// so slow motion is never lost. Axis lock (not on a Mac, as for Drag Scroll)
// holds the dominant axis until the fingers rest.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Pad pixels per detent at 1.0x. Windows and Linux: 170 keeps the feel the pad
// had at the default 800 DPI (0.70 counts per pixel up and down).
#ifndef SVAL_PAD_SCROLL_PX_PER_DETENT
#    define SVAL_PAD_SCROLL_PX_PER_DETENT 170
#endif
// macOS: 180 pixels (about 4 mm up and down) per line at 1.0x, after the pad's divider.
#ifndef SVAL_PAD_SCROLL_PX_PER_DETENT_MAC
#    define SVAL_PAD_SCROLL_PX_PER_DETENT_MAC 180
#endif
// Counts per line on a Mac: the 120 counts of a detent make one line.
#define SVAL_PAD_SCROLL_MAC_DIV 120
#define SVAL_PAD_SCROLL_HIRES 120
// Axis lock: pixels of motion that decide the axis, and the rest that frees it.
#define SVAL_PAD_SCROLL_LOCK_PX 24
#define SVAL_PAD_SCROLL_LOCK_REST_MS 250

typedef struct {
    int32_t  rem_h, rem_v; // the part of a count not yet sent
    int32_t  acc_h, acc_v; // motion since the axis was free
    uint8_t  lock;         // 0 free, 1 vertical, 2 horizontal
    bool     mac;          // the mode the remainders belong to
    uint32_t last_ms;
} sval_pad_scroll_t;

// Pad pixels (full resolution) in (h: right, v: as the chip reports), the
// hand's Scroll speed in percent (20 to 1000), wheel counts out.
void sval_pad_scroll(sval_pad_scroll_t *s, uint32_t now_ms, int32_t px_h, int32_t px_v, bool mac, bool lock, uint16_t speed_pct, int16_t *h, int16_t *v);
