// Host test for wheel_pace.c: at most one detent per axis per report, the
// total kept, the backlog bounded, a reversal answered at once, a Mac
// untouched, no report across a detent boundary. Run by test_detect.py.
#include <stdio.h>
#include <stdlib.h>
#include "wheel_pace.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static uint32_t lcg = 12345;
static int32_t  rnd(int32_t lo, int32_t hi) {
    lcg = lcg * 1664525u + 1013904223u;
    return lo + (int32_t)((lcg >> 8) % (uint32_t)(hi - lo + 1));
}

int main(void) {
    sval_wheel_pace_t p = {0};
    int16_t           h, v;

    // 420 counts in one report leave as 120, 120, 120, 60.
    const int16_t want[] = {120, 120, 120, 60, 0};
    for (int i = 0; i < 5; i++) {
        h = 0;
        v = i == 0 ? 420 : 0;
        sval_wheel_pace(&p, &h, &v, false);
        CHECK(v == want[i]);
        CHECK(h == 0);
    }

    // Reports as measured on Windows (median 420, up to 1510, every 10 ms of
    // 1 ms reports) in one direction: every report capped, the total kept.
    p              = (sval_wheel_pace_t){0};
    int64_t in_sum = 0, out_sum = 0, in_h = 0, out_h = 0;
    for (int ms = 0; ms < 20000; ms++) {
        h = 0;
        v = 0;
        if (ms % 10 == 0 && ms < 19000) {
            v = (int16_t)rnd(1, 1510);
            h = (int16_t)-rnd(0, 200);
        }
        in_sum += v;
        in_h += h;
        sval_wheel_pace(&p, &h, &v, false);
        CHECK(v >= -SVAL_WHEEL_PACE_MAX && v <= SVAL_WHEEL_PACE_MAX);
        CHECK(h >= -SVAL_WHEEL_PACE_MAX && h <= SVAL_WHEEL_PACE_MAX);
        out_sum += v;
        out_h += h;
    }
    CHECK(in_sum == out_sum);
    CHECK(in_h == out_h);
    CHECK(p.h == 0 && p.v == 0);

    // A fast spin at 1000% (1510 counts every 1 ms report): the backlog stops
    // at SVAL_WHEEL_PACE_BACKLOG and drains within 100 reports of stopping.
    p = (sval_wheel_pace_t){0};
    for (int ms = 0; ms < 2000; ms++) {
        h = 0;
        v = 1510;
        sval_wheel_pace(&p, &h, &v, false);
        CHECK(v == SVAL_WHEEL_PACE_MAX);
        CHECK(p.v <= SVAL_WHEEL_PACE_BACKLOG);
    }
    int drain = 0;
    for (;;) {
        h = v = 0;
        sval_wheel_pace(&p, &h, &v, false);
        if (v == 0) break;
        drain++;
    }
    CHECK(drain <= SVAL_WHEEL_PACE_BACKLOG / SVAL_WHEEL_PACE_MAX);
    printf("worst-case drain %d reports (%d ms at 1 ms polling)\n", drain, drain);

    // Turning back drops the old backlog: the first report already goes the new way.
    p = (sval_wheel_pace_t){0};
    h = 0;
    v = 1000;
    sval_wheel_pace(&p, &h, &v, false);
    h = 0;
    v = -10;
    sval_wheel_pace(&p, &h, &v, false);
    CHECK(v == -10);
    CHECK(p.v == 0);

    // A Mac: unchanged, and a backlog left from before is cleared.
    p = (sval_wheel_pace_t){0};
    h = 0;
    v = 500;
    sval_wheel_pace(&p, &h, &v, false);
    h = -300;
    v = 420;
    sval_wheel_pace(&p, &h, &v, true);
    CHECK(v == 420 && h == -300);
    CHECK(p.h == 0 && p.v == 0);

    // Never across a detent boundary: a partial 60, then 60, then whole detents,
    // so a terminal that keeps only one event per 120 loses nothing.
    p = (sval_wheel_pace_t){0};
    h = 0;
    v = 60;
    sval_wheel_pace(&p, &h, &v, false);
    CHECK(v == 60);
    const int16_t after[] = {60, 120, 120, 0};
    for (int i = 0; i < 4; i++) {
        h = 0;
        v = i == 0 ? 300 : 0;
        sval_wheel_pace(&p, &h, &v, false);
        CHECK(v == after[i]);
    }
    // A running sum of the reports (a terminal's total) lands on every multiple of 120.
    p          = (sval_wheel_pace_t){0};
    int32_t ts = 0, boundaries = 0, sent = 0;
    for (int ms = 0; ms < 5000; ms++) {
        h = 0;
        v = ms % 10 == 0 ? (int16_t)rnd(1, 700) : 0;
        sval_wheel_pace(&p, &h, &v, false);
        ts += v;
        sent += v;
        CHECK(ts <= SVAL_WHEEL_PACE_MAX);
        if (ts == SVAL_WHEEL_PACE_MAX) {
            ts = 0;
            boundaries++;
        }
    }
    CHECK(boundaries == sent / SVAL_WHEEL_PACE_MAX);

    if (failures) return 1;
    printf("wheel_pace OK\n");
    return 0;
}
