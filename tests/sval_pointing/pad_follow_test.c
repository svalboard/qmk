// Host test for pad_follow.c: a grabbed touchpad finger as relative counts at
// the hand's DPI. Run by test_detect.py.
#include <stdio.h>
#include <stdlib.h>
#include "pad_follow.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void) {
    sval_pad_follow_t f = {0};
    int16_t           dx, dy;

    // A new finger starts where it lands: no motion, wherever it is.
    sval_pad_follow(&f, 7, 0, 900, 0, 900, 800, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    // 800 DPI: 43 mm is 1354 counts, so 2048 pad counts become 1354.
    sval_pad_follow(&f, 7, 2048, 900, 2048, 900, 800, &dx, &dy);
    CHECK(dx == 1354 && dy == 0);
    // Slow motion carries: 100 steps of one pad count add up to 100 * 1354 / 2048.
    long sum = 0;
    for (int i = 1; i <= 100; i++) {
        sval_pad_follow(&f, 7, (uint16_t)(2048 + i), 900, (uint16_t)(2048 + i), 900, 800, &dx, &dy);
        sum += dx;
    }
    CHECK(sum == 100 * 1354 / 2048 || sum == 100 * 1354 / 2048 + 1);
    // Negative motion, and Y over 40 mm (1792 counts): 800 DPI is 1260 per pad.
    sval_pad_follow(&f, 7, 2148, 900 - 1792 / 2, 2148, 900 - 1792 / 2, 800, &dx, &dy);
    CHECK(dy == -630 && dx == 0);
    // A DPI above the pad's own resolution adds nothing (as the pad's mouse).
    sval_pad_follow(&f, 7, 2148 + 100, 4, 2148 + 100, 4, 12000, &dx, &dy);
    CHECK(dx == 100 && dy == 0);
    // Another finger (a new serial) never jumps, even far away.
    sval_pad_follow(&f, 8, 10, 10, 10, 10, 800, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    // A finger taken while already down starts from where it was a sample
    // before (its origin), so that sample's motion is not lost.
    sval_pad_follow(&f, 9, 2048 + 20, 10, 2048, 10, 800, &dx, &dy);
    CHECK(dx == 13 && dy == 0); // 20 * 1354 / 2048, the rest carried
    // No finger: nothing, and the next one starts fresh.
    sval_pad_follow(&f, 0, 0, 0, 0, 0, 800, &dx, &dy);
    CHECK(dx == 0 && dy == 0 && f.touch == 0);
    sval_pad_follow(&f, 8, 500, 500, 500, 500, 800, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    // The same finger moving on: 100 counts is 66 at this DPI (100 * 1354 / 2048).
    sval_pad_follow(&f, 8, 600, 500, 600, 500, 800, &dx, &dy);
    CHECK(dx == 66);
    // DPI 0 (no pointer): no motion, but the position is kept.
    sval_pad_follow(&f, 8, 700, 500, 700, 500, 0, &dx, &dy);
    CHECK(dx == 0 && f.x == 700);

    if (failures) return 1;
    puts("OK");
    return 0;
}
