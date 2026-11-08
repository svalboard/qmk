// Host test for scroll_speed.c (and pad_scroll.c): the trackball scaler at 800
// and 1600 DPI and 1.0x and 2.0x, Windows and Mac, the record, the migration
// that keeps every hand's output, and the pad DPI normalization. Run by
// test_detect.py.
#include <stdio.h>
#include <stdlib.h>
#include "scroll_speed.h"
#include "pad_scroll.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static const uint16_t choices[] = {200, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 12000};
#define N (sizeof(choices) / sizeof(choices[0]))

static uint32_t lcg = 12345;
static int32_t  rnd(int32_t lo, int32_t hi) {
    lcg = lcg * 1664525u + 1013904223u;
    return lo + (int32_t)((lcg >> 8) % (uint32_t)(hi - lo + 1));
}

// The scroll the firmware had before this (keymap_support.c add_to_axis with
// mult 1 and div 1, or 12 on a Mac): counts over the divisor, remainder carried.
static int16_t old_ball(int32_t counts, int div, int16_t *rem) {
    int32_t r   = *rem + counts;
    int16_t out = (int16_t)(r / div);
    *rem        = (int16_t)(r - out * div);
    return out;
}

// pad_scroll.c as PR 76 had it: the DPI scaled the pixels, capped at the pad's resolution.
static int16_t old_pad(int32_t px, bool mac, uint16_t dpi, uint16_t cap, int32_t *rem) {
    int64_t den = (mac ? 180 * 120 : 170) * 800;
    if (dpi > cap) dpi = cap;
    int64_t t = (int64_t)px * 120 * dpi + *rem;
    int64_t q = t / den;
    *rem      = (int32_t)(t - q * den);
    return (int16_t)q;
}

int main(void) {
    int16_t h, v;

    // ---- the range
    CHECK(sval_scroll_speed_clamp(0) == 20 && sval_scroll_speed_clamp(19) == 20);
    CHECK(sval_scroll_speed_clamp(20) == 20 && sval_scroll_speed_clamp(100) == 100);
    CHECK(sval_scroll_speed_clamp(1000) == 1000 && sval_scroll_speed_clamp(1001) == 1000 && sval_scroll_speed_clamp(65535) == 1000);

    // ---- the trackball at 1.0x is today's 800 DPI feel: one hi-res count per sensor count
    {
        sval_ball_scroll_t s = {0};
        sval_ball_scroll(&s, 5, -7, 800, 800, 100, false, &h, &v);
        CHECK(h == 5 && v == -7);
    }
    // ---- 800 and 1600 DPI, 1.0x and 2.0x, Windows and Mac: an inch of ball is the same scroll at any DPI
    static const uint16_t dpis[] = {800, 1600};
    static const uint16_t pcts[] = {100, 200};
    for (int mac = 0; mac < 2; mac++) {
        for (unsigned d = 0; d < 2; d++) {
            for (unsigned p = 0; p < 2; p++) {
                sval_ball_scroll_t s = {0};
                long               th = 0, tv = 0;
                // three inches in steps of 8 counts
                for (int i = 0; i < (int)dpis[d] * 3 / 8; i++) {
                    sval_ball_scroll(&s, 8, -8, dpis[d], dpis[d], pcts[p], mac, &h, &v);
                    th += h;
                    tv += v;
                }
                long want = 800L * 3 * pcts[p] / 100 / (mac ? 12 : 1);
                CHECK(th == want);
                CHECK(tv == -want);
            }
        }
    }
    // 2.0x is twice 1.0x on both systems, for the same motion, to the count.
    for (int mac = 0; mac < 2; mac++) {
        sval_ball_scroll_t a = {0}, b = {0};
        long               ta = 0, tb = 0;
        for (int i = 0; i < 2400; i++) {
            sval_ball_scroll(&a, 8, 0, 800, 800, 100, mac, &h, &v);
            ta += h;
            sval_ball_scroll(&b, 8, 0, 800, 800, 200, mac, &h, &v);
            tb += h;
        }
        CHECK(ta > 0 && tb == 2 * ta);
    }
    // The pointer's DPI is no input: a Mac at 1600 DPI, 1.0x, one count at a time, is half a line each.
    {
        sval_ball_scroll_t s = {0};
        long               t = 0;
        for (int i = 0; i < 9600; i++) {
            sval_ball_scroll(&s, 1, 0, 1600, 1600, 100, true, &h, &v);
            t += h;
        }
        CHECK(t == 9600L / 2 / 12);
    }
    // Slow scroll is not lost: 0.2x at 800 DPI, one count a time, and the other sign.
    {
        sval_ball_scroll_t s = {0}, n = {0};
        long               t = 0, tn = 0;
        for (int i = 0; i < 5000; i++) {
            sval_ball_scroll(&s, 1, 0, 800, 800, 20, false, &h, &v);
            t += h;
            sval_ball_scroll(&n, -1, 0, 800, 800, 20, false, &h, &v);
            tn += h;
        }
        CHECK(t == 1000 && tn == -1000);
    }
    // A change of Scroll speed keeps the remainder; a change of DPI or system drops it.
    {
        sval_ball_scroll_t s = {0};
        long               t = 0;
        for (int i = 0; i < 2000; i++) {
            sval_ball_scroll(&s, 1, 0, 800, 800, i < 1000 ? 50 : 150, false, &h, &v);
            t += h;
        }
        CHECK(t == (1000 * 50 + 1000 * 150) / 100);
        sval_ball_scroll_t k = {0};
        sval_ball_scroll(&k, 1, 0, 1600, 1600, 100, false, &h, &v); // half a count carried
        CHECK(h == 0 && k.rem_x != 0);
        sval_ball_scroll(&k, 0, 0, 800, 1600, 100, false, &h, &v);
        CHECK(k.rem_x == 0);
        sval_ball_scroll(&k, 1, 0, 800, 1600, 100, false, &h, &v);
        sval_ball_scroll(&k, 0, 0, 800, 1600, 100, true, &h, &v);
        CHECK(k.rem_x == 0);
    }
    // Axes are separate: dpi_y and dpi_x each divide their own axis.
    {
        sval_ball_scroll_t s = {0};
        sval_ball_scroll(&s, 1000, 1000, 1000, 500, 100, false, &h, &v);
        CHECK(h == 800 && v == 1600);
    }

    // ---- the DPI that counts for scroll
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_BALL, 1600, false) == 1600);
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_BALL, 12000, true) == 12000);
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_PAD, 800, false) == 800);
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_PAD, 3200, false) == 1210);
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_PAD, 3200, true) == 1138);
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_TRACKPOINT, 3200, false) == 800); // its DPI does nothing
    CHECK(sval_scroll_motion_dpi(SVAL_SCROLL_KIND_BALL, 0, false) == 800);

    // ---- the saved record
    {
        uint8_t  rec[SVAL_SCROLL_SPEED_RECORD_BYTES];
        uint16_t l = 0, r = 0;
        sval_scroll_speed_record_write(150, 1000, rec);
        CHECK(sval_scroll_speed_record_read(rec, &l, &r) && l == 150 && r == 1000);
        sval_scroll_speed_record_write(5, 5000, rec); // clamped on the way in
        CHECK(sval_scroll_speed_record_read(rec, &l, &r) && l == 20 && r == 1000);
        rec[4] ^= 1;
        CHECK(!sval_scroll_speed_record_read(rec, &l, &r)); // a flipped check
        for (int fill = 0; fill < 2; fill++) { // erased (0x00 here, 0xFF on other flash) is no record
            for (int i = 0; i < SVAL_SCROLL_SPEED_RECORD_BYTES; i++) rec[i] = fill ? 0xFF : 0x00;
            CHECK(!sval_scroll_speed_record_read(rec, &l, &r));
        }
        sval_scroll_speed_record_write(100, 100, rec);
        rec[0] = 5; // out of range with a matching check
        rec[4] = (uint8_t)(0x5A ^ rec[0] ^ rec[1] ^ rec[2] ^ rec[3]);
        CHECK(!sval_scroll_speed_record_read(rec, &l, &r));
    }
    // The test build's record: the percent, then 0xA5 ^ low ^ high.
    {
        uint8_t  rec[3] = {150, 0, (uint8_t)(0xA5 ^ 150)};
        uint16_t pct    = 0;
        CHECK(sval_scroll_speed_legacy_read(rec, &pct) && pct == 150);
        uint8_t big[3] = {0x90, 0x01, (uint8_t)(0xA5 ^ 0x90 ^ 0x01)}; // 400
        CHECK(sval_scroll_speed_legacy_read(big, &pct) && pct == 400);
        uint8_t zero[3] = {0, 0, 0};
        CHECK(!sval_scroll_speed_legacy_read(zero, &pct));
        uint8_t bad[3] = {150, 0, 0};
        CHECK(!sval_scroll_speed_legacy_read(bad, &pct));
    }

    // ---- migration: the Scroll speed from the DPI
    static const uint16_t ball_pct[N] = {25, 50, 75, 100, 150, 200, 300, 400, 600, 800, 1000};
    for (unsigned i = 0; i < N; i++) CHECK(sval_scroll_speed_from_dpi(choices[i], SVAL_SCROLL_KIND_BALL) == ball_pct[i]);
    CHECK(sval_scroll_speed_from_dpi(12000, SVAL_SCROLL_KIND_BALL) == 1000); // 15x is above the range
    CHECK(sval_scroll_speed_from_dpi(1200, SVAL_SCROLL_KIND_PAD) == 150);
    CHECK(sval_scroll_speed_from_dpi(1600, SVAL_SCROLL_KIND_PAD) == 151); // capped at 1210 / 800
    CHECK(sval_scroll_speed_from_dpi(12000, SVAL_SCROLL_KIND_PAD) == 151);
    CHECK(sval_scroll_speed_from_dpi(400, SVAL_SCROLL_KIND_PAD) == 50);
    for (unsigned i = 0; i < N; i++) CHECK(sval_scroll_speed_from_dpi(choices[i], SVAL_SCROLL_KIND_TRACKPOINT) == 100);

    // Migration keeps a trackball's output exactly: the same stream of counts
    // gives the same wheel counts, step by step, at every DPI up to 8000.
    for (unsigned i = 0; i < N - 1; i++) {
        for (int mac = 0; mac < 2; mac++) {
            sval_ball_scroll_t s    = {0};
            int16_t            rx   = 0, ry = 0;
            uint16_t           pct;
            uint8_t            idx;
            sval_scroll_migrate(SVAL_SCROLL_KIND_BALL, (uint8_t)i, 0, choices, N, &pct, &idx);
            CHECK(idx == i);
            int bad = 0;
            for (int k = 0; k < 4000; k++) {
                int32_t x = rnd(-60, 60), y = rnd(-60, 60);
                sval_ball_scroll(&s, x, y, choices[i], choices[i], pct, mac, &h, &v);
                int16_t ox = old_ball(x, mac ? 12 : 1, &rx), oy = old_ball(y, mac ? 12 : 1, &ry);
                if (h != ox || v != oy) bad++;
            }
            CHECK(bad == 0);
        }
    }
    // The same for a touchpad up to its maximum, across (up and down it was capped at 1138 DPI, so at 1200 it
    // scrolls 5% faster up and down: one speed per hand).
    for (unsigned i = 0; i < 5; i++) { // 200 to 1200
        for (int mac = 0; mac < 2; mac++) {
            sval_pad_scroll_t s   = {0};
            int32_t           rv  = 0;
            uint16_t          pct;
            uint8_t           idx;
            sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, (uint8_t)i, 0, choices, N, &pct, &idx);
            CHECK(idx == i);
            int bad = 0;
            for (int k = 0; k < 4000; k++) {
                int32_t px = rnd(-90, 90);
                sval_pad_scroll(&s, (uint32_t)k, px, 0, mac, false, pct, &h, &v);
                if (h != old_pad(px, mac, choices[i], 1210, &rv)) bad++; // across: the 1210 cap
            }
            CHECK(bad == 0);
        }
    }
    // Above the pad's maximum the output was capped, and stays within 0.2% across (1210 / 800 is 151.25%).
    for (unsigned i = 5; i < N; i++) {
        sval_pad_scroll_t s  = {0};
        int32_t           rh = 0;
        long              tn = 0, to = 0;
        uint16_t          pct;
        uint8_t           idx;
        sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, (uint8_t)i, 0, choices, N, &pct, &idx);
        CHECK(pct == 151 && idx == 4);
        for (int k = 0; k < 5000; k++) {
            sval_pad_scroll(&s, (uint32_t)k, 17, 0, false, false, pct, &h, &v);
            tn += h;
            to += old_pad(17, false, choices[i], 1210, &rh);
        }
        CHECK(labs(tn - to) * 1000 <= to * 2);
    }
    // A touchpad that ran the test build keeps its percent; other hands do not use it.
    {
        uint16_t pct;
        uint8_t  idx;
        sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, 3, 175, choices, N, &pct, &idx);
        CHECK(pct == 175 && idx == 3);
        // The test build's percent multiplied the DPI's speed: 3200 caps at
        // 1210 (151%), x 1.75 = 264%; 1200 (150%) x 1.75 = 263%.
        sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, 7, 175, choices, N, &pct, &idx);
        CHECK(pct == 264 && idx == 4);
        sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, 4, 175, choices, N, &pct, &idx);
        CHECK(pct == 263 && idx == 4);
        sval_scroll_migrate(SVAL_SCROLL_KIND_BALL, 5, 175, choices, N, &pct, &idx);
        CHECK(pct == 200 && idx == 5);
        sval_scroll_migrate(SVAL_SCROLL_KIND_TRACKPOINT, 5, 175, choices, N, &pct, &idx);
        CHECK(pct == 100 && idx == 5);
    }
    // A saved index past the table reads as the last choice, never out of bounds.
    {
        uint16_t pct;
        uint8_t  idx;
        sval_scroll_migrate(SVAL_SCROLL_KIND_BALL, 200, 0, choices, N, &pct, &idx);
        CHECK(pct == 1000);
    }

    // ---- pad DPI normalization
    for (unsigned i = 0; i < N; i++) {
        uint8_t want = i > 4 ? 4 : (uint8_t)i;
        CHECK(sval_dpi_index_normalize_pad(choices, N, (uint8_t)i) == want);
    }
    CHECK(sval_dpi_index_at_most(choices, N, 1210) == 4);
    CHECK(sval_dpi_index_at_most(choices, N, 1199) == 3);
    CHECK(sval_dpi_index_at_most(choices, N, 100) == 0);
    CHECK(sval_dpi_index_normalize_pad(choices, N, 4) == 4); // already at the maximum: stays, so a second boot changes nothing
    {
        // Running it twice gives the same: the first result is a fixed point.
        for (unsigned i = 0; i < N; i++) {
            uint16_t p1, p2;
            uint8_t  i1, i2;
            sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, (uint8_t)i, 0, choices, N, &p1, &i1);
            sval_scroll_migrate(SVAL_SCROLL_KIND_PAD, i1, 0, choices, N, &p2, &i2);
            CHECK(i2 == i1);
            (void)p2;
        }
    }

    if (failures == 0) printf("scroll_speed OK\n");
    return failures ? 1 : 0;
}
