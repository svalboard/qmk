// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_scroll.h"

// The remainder is in units of 1/den count, and den does not depend on the DPI,
// so it stays right when the DPI changes.
static int16_t counts(int32_t px, int64_t den, uint16_t dpi, int32_t *rem) {
    int64_t t = (int64_t)px * SVAL_PAD_SCROLL_HIRES * dpi + *rem;
    int64_t q = t / den;
    *rem      = (int32_t)(t - q * den);
    if (q > 32767) q = 32767;
    if (q < -32767) q = -32767;
    return (int16_t)q;
}

static int32_t iabs(int32_t v) {
    return v < 0 ? -v : v;
}

void sval_pad_scroll(sval_pad_scroll_t *s, uint32_t now_ms, int32_t px_h, int32_t px_v, bool mac, bool lock, uint16_t dpi, int16_t *h, int16_t *v) {
    *h = *v = 0;
    if (dpi == 0) dpi = SVAL_PAD_SCROLL_REF_DPI;
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
    // The px-per-detent distances hold at the reference DPI; the DPI scales them.
    int64_t den = (mac ? (int64_t)SVAL_PAD_SCROLL_PX_PER_DETENT_MAC * SVAL_PAD_SCROLL_MAC_DIV : SVAL_PAD_SCROLL_PX_PER_DETENT) * SVAL_PAD_SCROLL_REF_DPI;
    // The pointer stops gaining at the pad's full resolution (azoteq.c, azo_num),
    // so scroll does too: the DPI means the same for both, as on x4.5.
    *h          = counts(px_h, den, dpi < SVAL_PAD_SCROLL_CAP_DPI_X ? dpi : SVAL_PAD_SCROLL_CAP_DPI_X, &s->rem_h);
    *v          = counts(px_v, den, dpi < SVAL_PAD_SCROLL_CAP_DPI_Y ? dpi : SVAL_PAD_SCROLL_CAP_DPI_Y, &s->rem_v);
}
