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

void sval_pad_touch_size(sval_pad_touch_t *t, uint8_t fingers, uint16_t strength, uint8_t area) {
    t->strength = fingers ? strength : 0;
    t->area     = fingers ? area : 0;
}

void sval_pad_touch_pack_size(const sval_pad_touch_t *t, uint8_t out[SVAL_TOUCH_SIZE_BYTES]) {
    put16(out, t->live ? t->strength : 0);
    out[2] = t->live ? t->area : 0;
}

void sval_pad_touch_merge(uint8_t held[SVAL_TOUCH_BLOCK_BYTES], const uint8_t fresh[SVAL_TOUCH_BLOCK_BYTES]) {
    const uint8_t events = held[1] | fresh[1];
    memcpy(held, fresh, SVAL_TOUCH_BLOCK_BYTES);
    held[1] = events;
}

// ---------------------------------------------------------------- every finger

static uint8_t popcount5(uint8_t m) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < SVAL_FINGERS_MAX; i++)
        n += (m >> i) & 1;
    return n;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)p[0] << 8 | p[1];
}

// A distance in pixels as 0.01 mm, each axis at its own scale.
static uint16_t px_dist_mm100(const sval_pad_touch_cfg_t *cfg, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    const uint16_t dx = sval_pad_px_to_mm100(absdiff(x0, x1), cfg->xres, SVAL_PAD_WIDTH_MM100);
    const uint16_t dy = sval_pad_px_to_mm100(absdiff(y0, y1), cfg->yres, SVAL_PAD_HEIGHT_MM100);
    return isqrt32((uint32_t)dx * dx + (uint32_t)dy * dy);
}

void sval_pad_fingers_sample(sval_pad_fingers_t *f, const sval_pad_touch_cfg_t *cfg, bool valid, const uint8_t *raw, uint32_t now_ms) {
    if (!valid || raw[5] > SVAL_FINGERS_MAX) return;
    f->live   = true;
    f->status = ((raw[4] & 0x02) ? SVAL_FINGERS_PALM : 0) | ((raw[4] & 0x04) ? SVAL_FINGERS_TOO_MANY : 0);
    // A slot holds a finger while its strength is nonzero (multitouch.c reads them the same way).
    uint8_t mask = 0;
    for (uint8_t i = 0; i < SVAL_FINGERS_MAX; i++) {
        const uint8_t *p = raw + 10 + 7 * i;
        if (!raw[5] || !(p[4] || p[5])) continue;
        mask |= 1u << i;
        f->x[i]    = be16(p);
        f->y[i]    = be16(p + 2);
        f->area[i] = p[6];
    }
    f->mask         = mask;
    const uint8_t n = popcount5(mask);
    // The chip's two-finger gestures (datasheet 6.5-6.7) need two touches, and
    // only two, all through: one that lands next to a single finger starts
    // one; a third finger, or a lift, ends it, and only a new touch starts
    // another. Scroll starts once the two touches' average travel passes the
    // scroll distance.
    if (n == 2 && f->prev_n < 2) {
        f->two = f->had_two = true;
        f->scrolled         = false;
        f->two_mask         = mask;
        f->two_at           = now_ms;
        f->two_ms           = 0;
        f->travel           = 0;
        uint8_t k           = 0;
        for (uint8_t i = 0; i < SVAL_FINGERS_MAX && k < 2; i++)
            if (mask & (1u << i)) {
                f->sx[k]   = f->x[i];
                f->sy[k++] = f->y[i];
            }
    } else if (f->two && (n != 2 || mask != f->two_mask)) {
        f->two = false;
    }
    if (f->two) {
        uint32_t sum = 0;
        uint8_t  k   = 0;
        for (uint8_t i = 0; i < SVAL_FINGERS_MAX && k < 2; i++)
            if (mask & (1u << i)) {
                sum += px_dist_mm100(cfg, f->sx[k], f->sy[k], f->x[i], f->y[i]);
                k++;
            }
        f->travel              = (uint16_t)(sum / 2);
        const uint32_t elapsed = now_ms - f->two_at;
        f->two_ms              = elapsed > UINT16_MAX ? UINT16_MAX : (uint16_t)elapsed;
    }
    if ((raw[2] & 0x02) && f->had_two) f->scrolled = true;
    if (raw[2] & 0x01) f->events |= SVAL_FINGERS_TWO_TAP;
    f->prev_n = n;
}

// A position in pixels as 0.02 mm, 12 bits per axis, 24-bit little-endian.
static void put_pos(uint8_t *p, const sval_pad_touch_cfg_t *cfg, uint16_t x, uint16_t y) {
    uint32_t mx = (sval_pad_px_to_mm100(x, cfg->xres, SVAL_PAD_WIDTH_MM100) + 1) / 2;
    uint32_t my = (sval_pad_px_to_mm100(y, cfg->yres, SVAL_PAD_HEIGHT_MM100) + 1) / 2;
    if (mx > 0xFFF) mx = 0xFFF;
    if (my > 0xFFF) my = 0xFFF;
    const uint32_t v = mx | my << 12;
    p[0]             = v & 0xFF;
    p[1]             = (v >> 8) & 0xFF;
    p[2]             = (v >> 16) & 0xFF;
}

void sval_pad_fingers_pack(sval_pad_fingers_t *f, const sval_pad_touch_cfg_t *cfg, uint32_t now_ms, uint8_t out[SVAL_FINGERS_BLOCK_BYTES]) {
    (void)now_ms;
    memset(out, 0, SVAL_FINGERS_BLOCK_BYTES);
    if (!f->live) return;
    out[0] = SVAL_FINGERS_VALID | f->status | (f->two ? SVAL_FINGERS_TWO : 0) | (f->scrolled ? SVAL_FINGERS_SCROLL : 0) | f->events | (f->had_two ? SVAL_FINGERS_HAD_TWO : 0);
    out[1] = f->mask;
    for (uint8_t i = 0; i < SVAL_FINGERS_MAX; i++) {
        if (!(f->mask & (1u << i))) continue;
        put_pos(&out[2 + 4 * i], cfg, f->x[i], f->y[i]);
        out[5 + 4 * i] = f->area[i];
    }
    if (f->had_two) {
        put_pos(&out[22], cfg, (uint16_t)((f->sx[0] + f->sx[1] + 1) / 2), (uint16_t)((f->sy[0] + f->sy[1] + 1) / 2));
        const uint16_t t = (f->travel + 2) / 5;
        out[25]          = t > 255 ? 255 : (uint8_t)t;
        const uint16_t q = f->two_ms / 4;
        out[26]          = q > 255 ? 255 : (uint8_t)q;
    }
    f->events = 0;
}

void sval_pad_fingers_merge(uint8_t held[SVAL_FINGERS_BLOCK_BYTES], const uint8_t fresh[SVAL_FINGERS_BLOCK_BYTES]) {
    const uint8_t tap = held[0] & SVAL_FINGERS_TWO_TAP;
    memcpy(held, fresh, SVAL_FINGERS_BLOCK_BYTES);
    if (held[0] & SVAL_FINGERS_VALID) held[0] |= tap;
}
