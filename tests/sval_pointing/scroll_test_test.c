// scroll_test.c (SVAL_SCROLL_TEST, dev builds): the injected motion keeps its
// direction and its total for any int32 rate (F30: the remainder overflowed
// int32 above about 2,000,000 counts per second).
#include <stdio.h>
#include <string.h>
#include "scroll_test.h"

uint32_t fake_now = 1000;

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static void start(int32_t rate, uint16_t ms) {
    uint8_t d[32] = {0};
    d[7]          = 1;
    d[8]          = 0;
    memcpy(d + 9, &rate, 4);
    d[13] = ms & 0xFF;
    d[14] = ms >> 8;
    sval_scroll_test_command(d, sizeof(d));
}

static int32_t total(void) {
    uint8_t d[32] = {0};
    d[7]          = 2; // stop, and read the total
    sval_scroll_test_command(d, sizeof(d));
    int32_t t;
    memcpy(&t, d + 8, 4);
    return t;
}

// Runs a segment with a pointing task every step ms; returns the sum of y.
static int64_t run(int32_t rate, uint16_t ms, uint32_t step, int *wrong_sign) {
    start(rate, ms);
    int64_t sum = 0;
    *wrong_sign = 0;
    for (uint32_t t = 0; t < ms; t += step) {
        report_mouse_t l = {0}, r = {0};
        fake_now += step;
        sval_scroll_test_motion(&l, &r);
        if ((rate > 0 && l.y < 0) || (rate < 0 && l.y > 0)) (*wrong_sign)++;
        sum += l.y;
    }
    return sum;
}

int main(void) {
    int     wrong;
    int64_t sum;
    // An ordinary rate: exact (the last task ends the segment).
    sum = run(1200, 5000, 1, &wrong);
    CHECK(wrong == 0 && sum == 5998);
    // 3,000,000 counts/s runs past the 2000 counts a report carries: always up, never reversed.
    sum = run(3000000, 5000, 1, &wrong);
    CHECK(wrong == 0 && sum > 0);
    CHECK(total() == (int32_t)sum);
    // The largest rates both ways, and tasks 10 ms apart.
    sum = run(INT32_MAX, 3000, 10, &wrong);
    CHECK(wrong == 0 && sum > 0 && total() == (int32_t)sum);
    sum = run(INT32_MIN, 3000, 1, &wrong);
    CHECK(wrong == 0 && sum < 0 && total() == (int32_t)sum);
    if (failures) return 1;
    printf("scroll_test OK\n");
    return 0;
}
