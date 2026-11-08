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
    sval_pad_scroll(&s, 0, 0, 170, false, false, 100, &h, &v);
    CHECK(h == 0 && v == 120);
    sval_pad_scroll(&s, 10, 0, -85, false, false, 100, &h, &v);
    CHECK(v == -60);
    // Mac: 180 pixels per count (a line), 1000 pixels in steps of 7 carry.
    sval_pad_scroll_t m = {0};
    long              sum = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&m, (uint32_t)i, 0, 7, true, false, 100, &h, &v);
        CHECK(v >= 0 && v <= 1);
        sum += v;
    }
    CHECK(sum == 1800L * 7 / 180);
    // Slow motion is not lost: one pixel at a time.
    sval_pad_scroll_t k = {0};
    sum                 = 0;
    for (int i = 0; i < 1700; i++) {
        sval_pad_scroll(&k, (uint32_t)i, 1, 0, false, false, 100, &h, &v);
        sum += h;
    }
    CHECK(sum == 1200);
    // Negative motion carries symmetrically.
    sval_pad_scroll_t n = {0};
    sum                 = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&n, (uint32_t)i, 0, -7, true, false, 100, &h, &v);
        sum += v;
    }
    CHECK(sum == -(1800L * 7 / 180));
    // A change of mode drops the remainders of the mode before.
    sval_pad_scroll_t c = {0};
    sval_pad_scroll(&c, 0, 0, 100, false, false, 100, &h, &v);
    sval_pad_scroll(&c, 1, 0, 0, true, false, 100, &h, &v);
    sval_pad_scroll(&c, 2, 0, 1, true, false, 100, &h, &v);
    CHECK(v == 0 && c.rem_v == 120 * 100);
    // Axis lock: vertical-dominant motion drops the sideways part until rest.
    sval_pad_scroll_t l = {0};
    sval_pad_scroll(&l, 0, 20, 30, false, true, 100, &h, &v); // decides V
    sval_pad_scroll(&l, 10, 20, 30, false, true, 100, &h, &v);
    CHECK(h == 0 && v > 0);
    // Rest frees it: now horizontal wins.
    sval_pad_scroll(&l, 1000, 40, 5, false, true, 100, &h, &v);
    CHECK(h > 0 && v == 0); // horizontal wins on this report
    sval_pad_scroll(&l, 1010, 10, 4, false, true, 100, &h, &v);
    CHECK(h > 0 && v == 0);
    // No lock when off.
    sval_pad_scroll(&l, 1020, 10, 4, false, false, 100, &h, &v);
    CHECK(h > 0 && v > 0);
    // The Scroll speed multiplies, and nothing else does: counts are
    // proportional to the percent. 1700 pixels at 1.0x are ten detents (1200
    // counts) on Windows and 1800 pixels are ten lines on a Mac. The Mac
    // divider applies after the multiplier, so 2.0x doubles both.
    static const uint16_t speeds[] = {20, 25, 50, 100, 150, 200, 1000};
    for (unsigned d = 0; d < sizeof(speeds) / sizeof(speeds[0]); d++) {
        for (int mac = 0; mac < 2; mac++) {
            sval_pad_scroll_t x     = {0};
            long              total = 0;
            int               px    = mac ? 180 : 170;
            for (int i = 0; i < 1000; i++) {
                sval_pad_scroll(&x, (uint32_t)i, 0, px, mac, false, speeds[d], &h, &v);
                total += v;
            }
            long want = (mac ? 1000L : 1000L * 120) * speeds[d] / 100;
            CHECK(total == want);
        }
    }
    // 2.0x is exactly twice 1.0x, on both systems, for the same motion.
    for (int mac = 0; mac < 2; mac++) {
        sval_pad_scroll_t one = {0}, two = {0};
        long              a1 = 0, a2 = 0;
        for (int i = 0; i < 500; i++) {
            sval_pad_scroll(&one, (uint32_t)i, 0, 13, mac, false, 100, &h, &v);
            a1 += v;
            sval_pad_scroll(&two, (uint32_t)i, 0, 13, mac, false, 200, &h, &v);
            a2 += v;
        }
        CHECK(a1 > 0 && a2 == 2 * a1);
    }
    // The remainder survives a change of speed, and one pixel at a time at
    // 0.5x is not lost.
    sval_pad_scroll_t r = {0};
    sum                 = 0;
    for (int i = 0; i < 3400; i++) {
        sval_pad_scroll(&r, (uint32_t)i, 1, 0, false, false, 50, &h, &v);
        sum += h;
    }
    CHECK(sum == 1200);
    sval_pad_scroll_t w = {0};
    sum                 = 0;
    for (int i = 0; i < 1700; i++) {
        sval_pad_scroll(&w, (uint32_t)i, 1, 0, false, false, i < 850 ? 50 : 150, &h, &v);
        sum += h;
    }
    CHECK(sum == 120 * (850 * 50 + 850 * 150) / (170 * 100)); // exact: the carried part is not dropped
    // The pointer's DPI is not an input: the same pixels give the same counts.
    // Out-of-range speeds clamp (20 to 1000); 0 means 1.0x.
    sval_pad_scroll_t q = {0};
    sval_pad_scroll(&q, 0, 1700, 0, false, false, 5000, &h, &v);
    CHECK(h == 1700 * 120 * 1000 / (170 * 100));
    sval_pad_scroll_t lo = {0};
    sval_pad_scroll(&lo, 0, 1700, 0, false, false, 1, &h, &v);
    CHECK(h == 1700 * 120 * 20 / (170 * 100));
    sval_pad_scroll_t z = {0};
    sval_pad_scroll(&z, 0, 0, 170, false, false, 0, &h, &v);
    CHECK(v == 120);
    if (failures == 0) printf("pad_scroll OK\n");
    return failures ? 1 : 0;
}
