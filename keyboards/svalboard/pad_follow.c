// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_follow.h"

// Counts per pad at this DPI, as the chip's resolution register would be set
// (azoteq/azoteq.c, AZO_RES): never more than the pad's full resolution.
static uint32_t res(uint16_t dpi, uint32_t mm, uint32_t full) {
    uint32_t r = ((uint32_t)dpi * mm * 10u + 127u) / 254u;
    return r < full ? r : full;
}

static int16_t scale(int32_t d, uint32_t num, uint32_t den, int32_t *rem) {
    int32_t t = d * (int32_t)num + *rem;
    int32_t q = t / (int32_t)den;
    *rem      = t - q * (int32_t)den;
    if (q > 32767) q = 32767;
    if (q < -32767) q = -32767;
    return (int16_t)q;
}

void sval_pad_follow(sval_pad_follow_t *f, uint16_t touch, uint16_t x, uint16_t y, uint16_t x0, uint16_t y0, uint16_t dpi, int16_t *dx, int16_t *dy) {
    *dx = *dy = 0;
    if (!touch) {
        f->touch = 0;
        return;
    }
    if (touch != f->touch) { // a new finger: its motion starts at its origin
        f->touch = touch;
        f->x     = x0;
        f->y     = y0;
        f->rem_x = f->rem_y = 0;
    }
    int32_t mx = (int32_t)x - f->x, my = (int32_t)y - f->y;
    f->x = x;
    f->y = y;
    if (!dpi) return;
    *dx = scale(mx, res(dpi, SVAL_PAD_FOLLOW_MM_X, SVAL_PAD_FOLLOW_FULL_X), SVAL_PAD_FOLLOW_FULL_X, &f->rem_x);
    *dy = scale(my, res(dpi, SVAL_PAD_FOLLOW_MM_Y, SVAL_PAD_FOLLOW_FULL_Y), SVAL_PAD_FOLLOW_FULL_Y, &f->rem_y);
}
