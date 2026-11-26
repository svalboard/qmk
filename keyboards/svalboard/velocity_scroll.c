// SPDX-License-Identifier: GPL-2.0-or-later
// Velocity Scroll math (velocity_scroll.h). No QMK headers: tests/sval_pointing
// builds this on the host.
#include <math.h>
#include <string.h>
#include "velocity_scroll.h"

float sval_vs_speed(const sval_vs_curve_t *c, float offset_mm) {
    float d    = fabsf(offset_mm);
    float past = d - c->dead_mm;
    if (past <= 0.0f) return 0.0f;
    float nps = c->gain * powf(past / 10.0f, c->exponent);
    if (nps > c->max_nps) nps = c->max_nps;
    return offset_mm < 0.0f ? -nps : nps;
}

float sval_vs_full_mm(const sval_vs_curve_t *c) {
    if (c->gain <= 0.0f || c->exponent <= 0.0f) return c->dead_mm;
    return c->dead_mm + 10.0f * powf(c->max_nps / c->gain, 1.0f / c->exponent);
}

void sval_vs_start(sval_vs_t *s) {
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->pad    = SVAL_VS_NO_PAD;
}

void sval_vs_stop(sval_vs_t *s) {
    memset(s, 0, sizeof(*s));
    s->pad = SVAL_VS_NO_PAD;
}

bool sval_vs_hold_on(const sval_vs_hold_t *h) {
    return h->keys || h->toggled || h->dev;
}

void sval_vs_key(sval_vs_hold_t *h, bool toggle, bool pressed) {
    if (toggle) {
        if (pressed) h->toggled = !h->toggled;
    } else if (pressed) {
        if (h->keys < 255) h->keys++;
    } else if (h->keys > 0) {
        h->keys--;
    }
}

int8_t sval_vs_apply(sval_vs_t *s, const sval_vs_hold_t *h) {
    bool on = sval_vs_hold_on(h);
    if (on == s->active) return 0;
    if (on) {
        sval_vs_start(s);
        return 1;
    }
    sval_vs_stop(s);
    return -1;
}

static float clampf(float v, float limit) {
    if (limit < 0.0f) return v;
    return v > limit ? limit : v < -limit ? -limit : v;
}

void sval_vs_add_relative(sval_vs_t *s, float dx_mm, float dy_mm, float clamp_mm) {
    if (!s->active) return;
    s->rel_x = clampf(s->rel_x + dx_mm, clamp_mm);
    s->rel_y = clampf(s->rel_y + dy_mm, clamp_mm);
}

// The finger in use is gone: stop until a newer touch lands, on either pad.
static void end_touch(sval_vs_t *s) {
    s->pad          = SVAL_VS_NO_PAD;
    s->touch        = 0;
    s->accept_after = s->newest;
    s->frac_h = s->frac_v = 0.0f;
    s->lock               = 0;
}

void sval_vs_pad(sval_vs_t *s, uint8_t pad, uint16_t touch, float x_mm, float y_mm) {
    if (!s->active) return;
    if (s->pad == pad) {
        if (touch == s->touch) {
            s->pad_x = x_mm;
            s->pad_y = y_mm;
            return;
        }
        end_touch(s); // lifted, or another finger took its place: that one may start again
    }
    if (touch && (!s->newest || (uint16_t)(touch - s->newest) < 0x8000u)) s->newest = touch;
    if (s->pad != SVAL_VS_NO_PAD || !touch) return;
    // A touch newer than the last one that ended (any touch, right after key-down).
    if (s->accept_after && (uint16_t)(touch - s->accept_after) - 1u >= 0x7FFFu) return;
    s->pad      = pad;
    s->touch    = touch;
    s->anchor_x = s->pad_x = x_mm;
    s->anchor_y = s->pad_y = y_mm;
    s->frac_h = s->frac_v = 0.0f;
    s->lock               = 0;
}

bool sval_vs_touching(const sval_vs_t *s) {
    return s->active && s->pad != SVAL_VS_NO_PAD;
}

void sval_vs_offset(const sval_vs_t *s, float *x_mm, float *y_mm) {
    bool touching = s->pad != SVAL_VS_NO_PAD;
    *x_mm         = !s->active ? 0.0f : touching ? s->pad_x - s->anchor_x : s->rel_x;
    *y_mm         = !s->active ? 0.0f : touching ? s->pad_y - s->anchor_y : s->rel_y;
}

static int16_t take_units(float *acc) {
    float whole = truncf(*acc);
    if (whole > 32767.0f) whole = 32767.0f;
    if (whole < -32767.0f) whole = -32767.0f;
    *acc -= whole;
    return (int16_t)whole;
}

float sval_vs_units_per_notch(uint16_t hires, bool mac) {
    if (mac) return (float)SVAL_VS_MAC_UNITS_PER_NOTCH;
    return hires ? (float)hires : 1.0f;
}

void sval_vs_step(sval_vs_t *s, const sval_vs_curve_t *ball, const sval_vs_curve_t *pad, bool axis_lock, float dt_ms, float units_per_notch, int16_t *h, int16_t *v) {
    *h = *v = 0;
    if (!s->active) return;
    bool                   touching = s->pad != SVAL_VS_NO_PAD;
    const sval_vs_curve_t *c        = touching ? pad : ball;
    float                  ox, oy;
    sval_vs_offset(s, &ox, &oy);
    float                  sx       = sval_vs_speed(c, ox);
    float                  sy       = sval_vs_speed(c, oy);
    if (axis_lock) {
        if (sx == 0.0f && sy == 0.0f)
            s->lock = 0;
        else if (!s->lock)
            s->lock = (fabsf(oy) - c->dead_mm >= fabsf(ox) - c->dead_mm) ? 1 : 2;
        if (s->lock == 1) sx = 0.0f;
        if (s->lock == 2) sy = 0.0f;
    }
    // A stopped axis drops its fraction, so it never sends a stray unit later.
    if (sx == 0.0f) s->frac_h = 0.0f;
    if (sy == 0.0f) s->frac_v = 0.0f;
    s->speed_h = sx;
    s->speed_v = -sy;
    if (dt_ms > 1000.0f) dt_ms = 1000.0f; // a stall never turns into a burst
    float k = units_per_notch * dt_ms / 1000.0f;
    s->frac_h += sx * k;
    s->frac_v -= sy * k; // toward the user (+y) scrolls down (wheel v < 0)
    *h = take_units(&s->frac_h);
    *v = take_units(&s->frac_v);
}
