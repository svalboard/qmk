// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// One pad's touch for the touch page (pad_touch.h). Pure logic, host tested
// in tests/sval_pad_touch.
#include "pad_touch.h"
#include "pad_gestures.h"
#include <string.h>

static uint16_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1ul << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r > UINT16_MAX ? UINT16_MAX : (uint16_t)r;
}

static uint16_t absdiff(uint16_t a, uint16_t b) {
    return a > b ? a - b : b - a;
}

void sval_pad_touch_sample(sval_pad_touch_t *t, const sval_pad_touch_cfg_t *cfg, bool valid, uint8_t fingers, uint8_t ge0, uint8_t ge1, uint16_t x, uint16_t y, uint32_t now_ms) {
    if (!valid || fingers > 5) return;
    t->live = true;
    if (fingers && !t->touching) {
        t->touching = true;
        t->outside  = false;
        t->farthest = 0;
        t->down_x = t->x = x;
        t->down_y = t->y = y;
        t->down_at       = now_ms;
        t->events |= SVAL_TOUCH_EV_DOWN;
    }
    if (fingers) {
        t->x              = x;
        t->y              = y;
        const uint16_t dx = absdiff(x, t->down_x), dy = absdiff(y, t->down_y);
        const uint16_t dxmm = sval_pad_px_to_mm100(dx, cfg->xres, SVAL_PAD_WIDTH_MM100);
        const uint16_t dymm = sval_pad_px_to_mm100(dy, cfg->yres, SVAL_PAD_HEIGHT_MM100);
        const uint16_t d    = isqrt32((uint32_t)dxmm * dxmm + (uint32_t)dymm * dymm);
        if (d > t->farthest) t->farthest = d;
        // The chip counts the tap distance in pixels; this is the firmware's
        // own reading of it (Euclidean), not the chip's.
        if (!t->outside && (uint32_t)dx * dx + (uint32_t)dy * dy > (uint32_t)cfg->tap_px * cfg->tap_px) {
            t->outside             = true;
            const uint32_t elapsed = now_ms - t->down_at;
            const bool     tap_ok  = elapsed < cfg->tap_ms;
            const bool     hold_ok = cfg->hold_on && !t->holding && elapsed < (uint32_t)cfg->tap_ms + cfg->hold_ms;
            if (tap_ok || hold_ok) t->events |= SVAL_TOUCH_EV_DRIFT;
        }
    } else if (t->touching) {
        t->touching            = false;
        const uint32_t elapsed = now_ms - t->down_at;
        t->last_ms             = elapsed > UINT16_MAX ? UINT16_MAX : (uint16_t)elapsed;
        t->events |= SVAL_TOUCH_EV_UP;
    }
    t->fingers = fingers;
    if (ge0 & 0x01) t->events |= SVAL_TOUCH_EV_TAP;
    if (ge0 & 0x02) {
        if (!t->holding) t->events |= SVAL_TOUCH_EV_HOLD;
        t->holding = true;
    } else {
        t->holding = false;
    }
    if (ge0 & 0x3C) t->events |= SVAL_TOUCH_EV_OTHER;
    if (ge1 & 0x01) t->events |= SVAL_TOUCH_EV_TWO_TAP;
    if (ge1 & 0x02) t->events |= SVAL_TOUCH_EV_SCROLL;
    if (ge1 & 0x04) t->events |= SVAL_TOUCH_EV_OTHER;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

void sval_pad_touch_pack(sval_pad_touch_t *t, const sval_pad_touch_cfg_t *cfg, uint32_t now_ms, uint8_t out[SVAL_TOUCH_BLOCK_BYTES]) {
    memset(out, 0, SVAL_TOUCH_BLOCK_BYTES);
    if (!t->live) return;
    out[0] = SVAL_TOUCH_VALID | (t->touching ? SVAL_TOUCH_DOWN : 0) | (t->outside ? SVAL_TOUCH_OUTSIDE : 0) | (uint8_t)((t->fingers & 7) << 4) | (t->holding ? SVAL_TOUCH_HOLDING : 0);
    out[1] = t->events;
    put16(&out[2], sval_pad_px_to_mm100(t->x, cfg->xres, SVAL_PAD_WIDTH_MM100));
    put16(&out[4], sval_pad_px_to_mm100(t->y, cfg->yres, SVAL_PAD_HEIGHT_MM100));
    put16(&out[6], sval_pad_px_to_mm100(t->down_x, cfg->xres, SVAL_PAD_WIDTH_MM100));
    put16(&out[8], sval_pad_px_to_mm100(t->down_y, cfg->yres, SVAL_PAD_HEIGHT_MM100));
    uint32_t ms = t->touching ? now_ms - t->down_at : t->last_ms;
    put16(&out[10], ms > UINT16_MAX ? UINT16_MAX : (uint16_t)ms);
    put16(&out[12], t->farthest);
    put16(&out[14], cfg->tap_px);
    t->events = 0;
}

void sval_pad_touch_merge(uint8_t held[SVAL_TOUCH_BLOCK_BYTES], const uint8_t fresh[SVAL_TOUCH_BLOCK_BYTES]) {
    const uint8_t events = held[1] | fresh[1];
    memcpy(held, fresh, SVAL_TOUCH_BLOCK_BYTES);
    held[1] = events;
}
