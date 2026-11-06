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
    sval_pad_scroll(&s, 0, 0, 170, false, false, &h, &v);
    CHECK(h == 0 && v == 120);
    sval_pad_scroll(&s, 10, 0, -85, false, false, &h, &v);
    CHECK(v == -60);
    // Mac: 180 pixels per count (a line), 1000 pixels in steps of 7 carry.
    sval_pad_scroll_t m = {0};
    long              sum = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&m, (uint32_t)i, 0, 7, true, false, &h, &v);
        CHECK(v >= 0 && v <= 1);
        sum += v;
    }
    CHECK(sum == 1800L * 7 / 180);
    // Slow motion is not lost: one pixel at a time.
    sval_pad_scroll_t k = {0};
    sum                 = 0;
    for (int i = 0; i < 1700; i++) {
        sval_pad_scroll(&k, (uint32_t)i, 1, 0, false, false, &h, &v);
        sum += h;
    }
    CHECK(sum == 1200);
    // Negative motion carries symmetrically.
    sval_pad_scroll_t n = {0};
    sum                 = 0;
    for (int i = 0; i < 1800; i++) {
        sval_pad_scroll(&n, (uint32_t)i, 0, -7, true, false, &h, &v);
        sum += v;
    }
    CHECK(sum == -(1800L * 7 / 180));
    // A change of mode drops the remainders of the mode before.
    sval_pad_scroll_t c = {0};
    sval_pad_scroll(&c, 0, 0, 100, false, false, &h, &v);
    sval_pad_scroll(&c, 1, 0, 0, true, false, &h, &v);
    sval_pad_scroll(&c, 2, 0, 1, true, false, &h, &v);
    CHECK(v == 0 && c.rem_v == 120);
    // Axis lock: vertical-dominant motion drops the sideways part until rest.
    sval_pad_scroll_t l = {0};
    sval_pad_scroll(&l, 0, 20, 30, false, true, &h, &v); // decides V
    sval_pad_scroll(&l, 10, 20, 30, false, true, &h, &v);
    CHECK(h == 0 && v > 0);
    // Rest frees it: now horizontal wins.
    sval_pad_scroll(&l, 1000, 40, 5, false, true, &h, &v);
    CHECK(h > 0 && v == 0); // horizontal wins on this report
    sval_pad_scroll(&l, 1010, 10, 4, false, true, &h, &v);
    CHECK(h > 0 && v == 0);
    // No lock when off.
    sval_pad_scroll(&l, 1020, 10, 4, false, false, &h, &v);
    CHECK(h > 0 && v > 0);
    if (failures == 0) printf("pad_scroll OK\n");
    return failures ? 1 : 0;
}
