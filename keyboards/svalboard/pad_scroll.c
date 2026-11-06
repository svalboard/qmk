// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_scroll.h"

static int16_t counts(int32_t px, int32_t den, int32_t *rem) {
    int32_t t = px * SVAL_PAD_SCROLL_HIRES + *rem;
    int32_t q = t / den;
    *rem      = t - q * den;
    if (q > 32767) q = 32767;
    if (q < -32767) q = -32767;
    return (int16_t)q;
}

static int32_t iabs(int32_t v) {
    return v < 0 ? -v : v;
}

void sval_pad_scroll(sval_pad_scroll_t *s, uint32_t now_ms, int32_t px_h, int32_t px_v, bool mac, uint8_t mac_divisor, bool lock, int16_t *h, int16_t *v) {
    *h = *v = 0;
    if (px_h == 0 && px_v == 0) return;
    if (s->lock != 0 || s->acc_h || s->acc_v) {
        if (now_ms - s->last_ms > SVAL_PAD_SCROLL_LOCK_REST_MS) s->lock = 0, s->acc_h = s->acc_v = 0;
    }
    s->last_ms = now_ms;
    if (lock && s->lock == 0) {
        s->acc_h += px_h;
        s->acc_v += px_v;
        if (iabs(s->acc_h) + iabs(s->acc_v) >= SVAL_PAD_SCROLL_LOCK_PX) s->lock = iabs(s->acc_v) >= iabs(s->acc_h) ? 1 : 2;
    }
    if (!lock) s->lock = 0;
    if (s->lock == 1) px_h = 0;
    if (s->lock == 2) px_v = 0;
    int32_t den = mac ? (int32_t)SVAL_PAD_SCROLL_PX_PER_DETENT_MAC * (mac_divisor ? mac_divisor : 1) : SVAL_PAD_SCROLL_PX_PER_DETENT;
    *h          = counts(px_h, den, &s->rem_h);
    *v          = counts(px_v, den, &s->rem_v);
}
