// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad's two-finger scroll as wheel counts (keymaps/keymap_support.c).
//
// The Azoteq chip reports a two-finger scroll as pixel motion, at the pad's
// full resolution (about 48 pixels per mm). The wheel is declared at 120
// counts per detent (config.h, POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER).
// Windows applies that; macOS treats each count as a line, so on a Mac the
// counts are divided by the Mac divider (as Drag Scroll's are), and the pad
// takes its own distance per step. The distance is in pad pixels, so the DPI
// setting does not change it, the way Velocity Scroll's distance does not.
// Remainders carry, so slow motion is never lost. Axis lock (not on a Mac,
// as for Drag Scroll) holds the dominant axis until the fingers rest.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Pad pixels per detent. Windows: 120 is one count per pixel, the rate the
// pad has always had at full resolution (about 2.5 mm per detent).
#ifndef SVAL_PAD_SCROLL_PX_PER_DETENT
#    define SVAL_PAD_SCROLL_PX_PER_DETENT 120
#endif
// macOS: about 4 mm (192 pixels) per line, before the Mac divider.
#ifndef SVAL_PAD_SCROLL_PX_PER_DETENT_MAC
#    define SVAL_PAD_SCROLL_PX_PER_DETENT_MAC 192
#endif
#define SVAL_PAD_SCROLL_HIRES 120
// Axis lock: pixels of motion that decide the axis, and the rest that frees it.
#define SVAL_PAD_SCROLL_LOCK_PX 24
#define SVAL_PAD_SCROLL_LOCK_REST_MS 250

typedef struct {
    int32_t  rem_h, rem_v; // the part of a count not yet sent
    int32_t  acc_h, acc_v; // motion since the axis was free
    uint8_t  lock;         // 0 free, 1 vertical, 2 horizontal
    uint32_t last_ms;
} sval_pad_scroll_t;

// Pad pixels in (h: right, v: as the chip reports), wheel counts out.
void sval_pad_scroll(sval_pad_scroll_t *s, uint32_t now_ms, int32_t px_h, int32_t px_v, bool mac, uint8_t mac_divisor, bool lock, int16_t *h, int16_t *v);
