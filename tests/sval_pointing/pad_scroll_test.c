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
    // Windows: one count per pixel, 120 pixels one detent (120 counts).
    sval_pad_scroll(&s, 0, 0, 120, false, 120, false, &h, &v);
    CHECK(h == 0 && v == 120);
    sval_pad_scroll(&s, 10, 0, -50, false, 120, false, &h, &v);
    CHECK(v == -50);
    // Mac: 192 pixels per count (a line), 1000 pixels in steps of 7 carry.
    sval_pad_scroll_t m = {0};
    long              sum = 0;
    for (int i = 0; i < 1920; i++) {
        sval_pad_scroll(&m, (uint32_t)i, 0, 7, true, 120, false, &h, &v);
        CHECK(v >= 0 && v <= 1);
        sum += v;
    }
    CHECK(sum == 1920L * 7 / 192);
    // Slow motion is not lost: one pixel at a time.
    sval_pad_scroll_t k = {0};
    sum                 = 0;
    for (int i = 0; i < 960; i++) {
        sval_pad_scroll(&k, (uint32_t)i, 1, 0, false, 120, false, &h, &v);
        sum += h;
    }
    CHECK(sum == 960);
    // Negative motion carries symmetrically.
    sval_pad_scroll_t n = {0};
    sum                 = 0;
    for (int i = 0; i < 1920; i++) {
        sval_pad_scroll(&n, (uint32_t)i, 0, -7, true, 120, false, &h, &v);
        sum += v;
    }
    CHECK(sum == -(1920L * 7 / 192));
    // Axis lock: vertical-dominant motion drops the sideways part until rest.
    sval_pad_scroll_t l = {0};
    sval_pad_scroll(&l, 0, 20, 30, false, 120, true, &h, &v); // decides V
    sval_pad_scroll(&l, 10, 20, 30, false, 120, true, &h, &v);
    CHECK(h == 0 && v == 30);
    // Rest frees it: now horizontal wins.
    sval_pad_scroll(&l, 1000, 40, 5, false, 120, true, &h, &v);
    CHECK(h == 40 && v == 0); // horizontal wins on this report
    sval_pad_scroll(&l, 1010, 10, 4, false, 120, true, &h, &v);
    CHECK(h == 10 && v == 0);
    // No lock when off.
    sval_pad_scroll(&l, 1020, 10, 4, false, 120, false, &h, &v);
    CHECK(h == 10 && v == 4);
    if (failures == 0) printf("pad_scroll OK\n");
    return failures ? 1 : 0;
}
