// Host test for pad_scroll.c: a pad's two-finger scroll pixels as wheel
// counts, Windows and Mac, remainders, axis lock. Run by test_detect.py.
#include <stdio.h>
#include <stdlib.h>
#include "pad_scroll.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void) {
    sval_pad_scroll_t s = {0};
    int16_t           h, v;
    // Windows: 170 pixels are a detent (120 counts).
    sval_pad_scroll(&s, 0, 0, 170, false, false, 800, &h, &v);
    CHECK(h == 0 && v == 120);
    sval_pad_scroll(&s, 10, 0, -85, false, false, 800, &h, &v);
    CHECK(v == -60);
    // Mac: 180 pixels per count (a line), 1000 pixels in steps of 7 carry.
    sval_pad_scroll_t m = {0};
    long              sum = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&m, (uint32_t)i, 0, 7, true, false, 800, &h, &v);
        CHECK(v >= 0 && v <= 1);
        sum += v;
    }
    CHECK(sum == 1800L * 7 / 180);
    // Slow motion is not lost: one pixel at a time.
    sval_pad_scroll_t k = {0};
    sum                 = 0;
    for (int i = 0; i < 1700; i++) {
        sval_pad_scroll(&k, (uint32_t)i, 1, 0, false, false, 800, &h, &v);
        sum += h;
    }
    CHECK(sum == 1200);
    // Negative motion carries symmetrically.
    sval_pad_scroll_t n = {0};
    sum                 = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&n, (uint32_t)i, 0, -7, true, false, 800, &h, &v);
        sum += v;
    }
    CHECK(sum == -(1800L * 7 / 180));
    // A change of mode drops the remainders of the mode before.
    sval_pad_scroll_t c = {0};
    sval_pad_scroll(&c, 0, 0, 100, false, false, 800, &h, &v);
    sval_pad_scroll(&c, 1, 0, 0, true, false, 800, &h, &v);
    sval_pad_scroll(&c, 2, 0, 1, true, false, 800, &h, &v);
    CHECK(v == 0 && c.rem_v == 120 * 800);
    // Axis lock: vertical-dominant motion drops the sideways part until rest.
    sval_pad_scroll_t l = {0};
    sval_pad_scroll(&l, 0, 20, 30, false, true, 800, &h, &v); // decides V
    sval_pad_scroll(&l, 10, 20, 30, false, true, 800, &h, &v);
    CHECK(h == 0 && v > 0);
    // Rest frees it: now horizontal wins.
    sval_pad_scroll(&l, 1000, 40, 5, false, true, 800, &h, &v);
    CHECK(h > 0 && v == 0); // horizontal wins on this report
    sval_pad_scroll(&l, 1010, 10, 4, false, true, 800, &h, &v);
    CHECK(h > 0 && v == 0);
    // No lock when off.
    sval_pad_scroll(&l, 1020, 10, 4, false, false, 800, &h, &v);
    CHECK(h > 0 && v > 0);
    // The DPI drives the speed: counts are proportional to it, anchored at 800.
    // 1700 pixels at 800 DPI are ten detents (1200 counts) on Windows and
    // 1800 pixels are ten lines on a Mac.
    static const uint16_t dpis[] = {400, 800, 1600, 3200, 200};
    for (unsigned d = 0; d < sizeof(dpis) / sizeof(dpis[0]); d++) {
        for (int mac = 0; mac < 2; mac++) {
            sval_pad_scroll_t x = {0};
            long              total = 0;
            int               px    = mac ? 180 : 170;
            for (int i = 0; i < 1000; i++) {
                sval_pad_scroll(&x, (uint32_t)i, 0, px, mac, false, dpis[d], &h, &v);
                total += v;
            }
            long eff  = dpis[d] < 1138 ? dpis[d] : 1138; // the vertical cap
            long want = (mac ? 1000L : 1000L * 120) * eff / 800;
            CHECK(total == want);
        }
    }
    // The remainder survives a change of DPI, and one pixel at a time at 400
    // is not lost.
    sval_pad_scroll_t r = {0};
    sum                 = 0;
    for (int i = 0; i < 3400; i++) {
        sval_pad_scroll(&r, (uint32_t)i, 1, 0, false, false, 400, &h, &v);
        sum += h;
    }
    CHECK(sum == 1200);
    // Across caps at 1210: 1600 and 3200 count what 1210 does.
    static const uint16_t hd[] = {1210, 1600, 3200};
    for (int k = 0; k < 3; k++) {
        sval_pad_scroll_t q = {0};
        sval_pad_scroll(&q, 0, 1700, 0, false, false, hd[k], &h, &v);
        CHECK(h == 1700 * 120 * 1210 / (170 * 800));
    }
    // DPI 0 means the 800 anchor.
    sval_pad_scroll_t z = {0};
    sval_pad_scroll(&z, 0, 0, 170, false, false, 0, &h, &v);
    CHECK(v == 120);
    if (failures == 0) printf("pad_scroll OK\n");
    return failures ? 1 : 0;
}
