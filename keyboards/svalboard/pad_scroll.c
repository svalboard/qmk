// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_scroll.h"

// The remainder is in units of 1/den count, den = pixels per detent x 100 (the
// speed is a percent), and does not depend on the speed, so it stays right when
// the speed changes.
static int16_t counts(int32_t px, int64_t den, uint16_t pct, int32_t *rem) {
    int64_t t = (int64_t)px * SVAL_PAD_SCROLL_HIRES * pct + *rem;
    int64_t q = t / den;
    *rem      = (int32_t)(t - q * den);
    if (q > 32767) q = 32767;
    if (q < -32767) q = -32767;
    return (int16_t)q;
}

static int32_t iabs(int32_t v) {
    return v < 0 ? -v : v;
}

void sval_pad_scroll(sval_pad_scroll_t *s, uint32_t now_ms, int32_t px_h, int32_t px_v, bool mac, bool lock, uint16_t speed_pct, int16_t *h, int16_t *v) {
    *h = *v = 0;
    if (speed_pct < 20) speed_pct = speed_pct == 0 ? 100 : 20;
    if (speed_pct > 1000) speed_pct = 1000;
    if (px_h == 0 && px_v == 0) return;
    if (s->lock != 0 || s->acc_h || s->acc_v) {
        if (now_ms - s->last_ms > SVAL_PAD_SCROLL_LOCK_REST_MS) s->lock = 0, s->acc_h = s->acc_v = 0;
    }
    s->last_ms = now_ms;
    if (s->mac != mac) { // a part of a count belongs to the mode it came from
        s->mac   = mac;
        s->rem_h = s->rem_v = 0;
    }
    if (lock && s->lock == 0) {
        s->acc_h += px_h;
        s->acc_v += px_v;
        if (iabs(s->acc_h) + iabs(s->acc_v) >= SVAL_PAD_SCROLL_LOCK_PX) s->lock = iabs(s->acc_v) >= iabs(s->acc_h) ? 1 : 2;
    }
    if (!lock) s->lock = 0;
    if (s->lock == 1) px_h = 0;
    if (s->lock == 2) px_v = 0;
    // Pixels per detent hold at 1.0x; the Scroll speed multiplies, then the
    // Mac divider applies.
    int64_t den = (mac ? (int64_t)SVAL_PAD_SCROLL_PX_PER_DETENT_MAC * SVAL_PAD_SCROLL_MAC_DIV : SVAL_PAD_SCROLL_PX_PER_DETENT) * 100;
    *h          = counts(px_h, den, speed_pct, &s->rem_h);
    *v          = counts(px_v, den, speed_pct, &s->rem_v);
}
