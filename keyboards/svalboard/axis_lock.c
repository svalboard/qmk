// SPDX-License-Identifier: GPL-2.0-or-later
#include "axis_lock.h"

static int32_t iabs(int32_t v) {
    return v < 0 ? -v : v;
}

static bool favors(int32_t a, int32_t b) {
    return iabs(a) * SVAL_AXIS_DOMINANCE_NUMERATOR >= iabs(b) * SVAL_AXIS_DOMINANCE_DENOMINATOR;
}

uint8_t sval_axis_lock_mode(const sval_axis_lock_t *s, uint32_t now_ms) {
    return now_ms - s->last_ms > SVAL_AXIS_LOCK_REST_MS ? SVAL_AXIS_FREE : s->mode;
}

bool sval_axis_lock_step(sval_axis_lock_t *s, uint32_t now_ms, int32_t h, int32_t v) {
    // The rest is measured at the next motion: the caller only calls with
    // motion (as pad_scroll.c does it for a pad).
    if (h == 0 && v == 0) return false;
    if (now_ms - s->last_ms > SVAL_AXIS_LOCK_REST_MS) {
        s->mode  = SVAL_AXIS_FREE;
        s->acc_h = 0;
        s->acc_v = 0;
    }
    s->last_ms = now_ms;

    switch (s->mode) {
        case SVAL_AXIS_FREE:
            s->acc_h += h;
            s->acc_v += v;
            if (favors(s->acc_v, s->acc_h)) {
                s->mode  = SVAL_AXIS_LOCKED_V;
                s->acc_h = 0;
            } else if (iabs(s->acc_h) >= SVAL_AXIS_LOCK_ENGAGE) {
                s->mode  = SVAL_AXIS_LOCKED_H;
                s->acc_v = 0;
            }
            return false;
        case SVAL_AXIS_LOCKED_V:
            if (favors(v, h)) s->acc_h = 0;
            s->acc_h += h;
            if (iabs(s->acc_h) < SVAL_AXIS_LOCK_BREAKAWAY) return false;
            s->mode  = SVAL_AXIS_LOCKED_H;
            s->acc_v = 0;
            return true;
        default: // SVAL_AXIS_LOCKED_H
            if (favors(h, v)) s->acc_v = 0;
            s->acc_v += v;
            if (iabs(s->acc_v) < SVAL_AXIS_LOCK_BREAKAWAY) return false;
            s->mode  = SVAL_AXIS_LOCKED_V;
            s->acc_h = 0;
            return true;
    }
}
