// SPDX-License-Identifier: GPL-2.0-or-later
#include "scroll_speed.h"

uint16_t sval_scroll_speed_clamp(uint16_t pct) {
    return pct < SVAL_SCROLL_SPEED_MIN ? SVAL_SCROLL_SPEED_MIN : pct > SVAL_SCROLL_SPEED_MAX ? SVAL_SCROLL_SPEED_MAX : pct;
}

static uint8_t check_of(const uint8_t *b, uint8_t n, uint8_t seed) {
    for (uint8_t i = 0; i < n; i++) seed ^= b[i];
    return seed;
}

void sval_scroll_speed_record_write(uint16_t left, uint16_t right, uint8_t rec[SVAL_SCROLL_SPEED_RECORD_BYTES]) {
    left   = sval_scroll_speed_clamp(left);
    right  = sval_scroll_speed_clamp(right);
    rec[0] = left & 0xFF;
    rec[1] = left >> 8;
    rec[2] = right & 0xFF;
    rec[3] = right >> 8;
    rec[4] = check_of(rec, 4, 0x5A);
}

bool sval_scroll_speed_record_read(const uint8_t rec[SVAL_SCROLL_SPEED_RECORD_BYTES], uint16_t *left, uint16_t *right) {
    if (check_of(rec, 4, 0x5A) != rec[4]) return false;
    uint16_t l = (uint16_t)(rec[0] | (rec[1] << 8)), r = (uint16_t)(rec[2] | (rec[3] << 8));
    if (l < SVAL_SCROLL_SPEED_MIN || l > SVAL_SCROLL_SPEED_MAX || r < SVAL_SCROLL_SPEED_MIN || r > SVAL_SCROLL_SPEED_MAX) return false;
    *left  = l;
    *right = r;
    return true;
}

bool sval_scroll_speed_legacy_read(const uint8_t rec[SVAL_SCROLL_SPEED_LEGACY_BYTES], uint16_t *pct) {
    if (check_of(rec, 2, 0xA5) != rec[2]) return false;
    uint16_t v = (uint16_t)(rec[0] | (rec[1] << 8));
    if (v == 0) return false;
    *pct = sval_scroll_speed_clamp(v);
    return true;
}

uint16_t sval_scroll_speed_from_dpi(uint16_t dpi, uint8_t kind) {
    if (kind == SVAL_SCROLL_KIND_TRACKPOINT) return SVAL_SCROLL_SPEED_DEFAULT;
    if (kind == SVAL_SCROLL_KIND_PAD && dpi > SVAL_PAD_CAP_DPI_X) dpi = SVAL_PAD_CAP_DPI_X;
    // DPI / 800 as a percent, rounded to nearest. Every listed DPI is a
    // multiple of 8, so it is exact up to 8000 DPI.
    uint32_t pct = ((uint32_t)dpi * 100u + SVAL_BALL_SCROLL_BASE_DPI / 2) / SVAL_BALL_SCROLL_BASE_DPI;
    return sval_scroll_speed_clamp(pct > 0xFFFF ? 0xFFFF : (uint16_t)pct);
}

uint8_t sval_dpi_index_at_most(const uint16_t *choices, uint8_t n, uint16_t limit) {
    uint8_t best = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (choices[i] <= limit) best = i;
    }
    return best;
}

uint8_t sval_dpi_index_normalize_pad(const uint16_t *choices, uint8_t n, uint8_t index) {
    uint8_t max = sval_dpi_index_at_most(choices, n, SVAL_PAD_CAP_DPI_X);
    return index > max ? max : index;
}

uint16_t sval_scroll_motion_dpi(uint8_t kind, uint16_t dpi, bool vertical) {
    if (kind == SVAL_SCROLL_KIND_TRACKPOINT || dpi == 0) return SVAL_BALL_SCROLL_BASE_DPI;
    if (kind == SVAL_SCROLL_KIND_PAD) {
        uint16_t cap = vertical ? SVAL_PAD_CAP_DPI_Y : SVAL_PAD_CAP_DPI_X;
        if (dpi > cap) dpi = cap;
    }
    return dpi;
}

// counts x (800 / dpi) x pct / 100, divided by 12 on a Mac. The remainder is
// in units of 1/(dpi x 100 x div), so it is only kept while those hold.
static int16_t axis(int32_t counts, uint16_t dpi, uint16_t pct, bool mac, int32_t *rem) {
    int64_t den = (int64_t)dpi * 100 * (mac ? SVAL_BALL_SCROLL_MAC_DIV : 1);
    int64_t t   = (int64_t)counts * SVAL_BALL_SCROLL_BASE_DPI * pct + *rem;
    int64_t q   = t / den;
    *rem        = (int32_t)(t - q * den);
    if (q > 32767) q = 32767;
    if (q < -32767) q = -32767;
    return (int16_t)q;
}

void sval_ball_scroll(sval_ball_scroll_t *s, int32_t x, int32_t y, uint16_t dpi_x, uint16_t dpi_y, uint16_t pct, bool mac, int16_t *h, int16_t *v) {
    if (dpi_x == 0) dpi_x = SVAL_BALL_SCROLL_BASE_DPI;
    if (dpi_y == 0) dpi_y = SVAL_BALL_SCROLL_BASE_DPI;
    if (s->dpi_x != dpi_x || s->dpi_y != dpi_y || s->mac != mac) { // a part of a count belongs to the resolution it came from
        s->dpi_x = dpi_x;
        s->dpi_y = dpi_y;
        s->mac   = mac;
        s->rem_x = s->rem_y = 0;
    }
    pct = sval_scroll_speed_clamp(pct);
    *h  = axis(x, dpi_x, pct, mac, &s->rem_x);
    *v  = axis(y, dpi_y, pct, mac, &s->rem_y);
}

void sval_scroll_migrate(uint8_t kind, uint8_t dpi_index, uint16_t legacy_pct, const uint16_t *choices, uint8_t n, uint16_t *speed, uint8_t *new_index) {
    uint8_t idx = dpi_index >= n ? n - 1 : dpi_index;
    *speed      = sval_scroll_speed_from_dpi(choices[idx], kind);
    // The test build multiplied its percent on top of the DPI's speed.
    if (kind == SVAL_SCROLL_KIND_PAD && legacy_pct) {
        uint32_t pct = ((uint32_t)*speed * legacy_pct + 50) / 100;
        *speed       = sval_scroll_speed_clamp(pct > 0xFFFF ? 0xFFFF : (uint16_t)pct);
    }
    *new_index  = kind == SVAL_SCROLL_KIND_PAD ? sval_dpi_index_normalize_pad(choices, n, dpi_index) : dpi_index;
}
