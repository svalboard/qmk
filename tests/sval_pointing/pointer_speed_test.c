// Host test for pointer_speed.c: the TrackPoint's software Pointer speed (the
// default is the old motion exactly, remainders carry, scroll does not follow
// the speed) and each module's DPI maximum. Run by test_detect.py.
#include <stdio.h>
#include <stdlib.h>
#include "pointer_speed.h"
#include "scroll_speed.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

// The list as shipped: new entries only at the end.
static const uint16_t choices[] = {200, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 12000, 16000};
#define N ((uint8_t)(sizeof(choices) / sizeof(choices[0])))

static uint32_t lcg = 777;
static int32_t  rnd(int32_t lo, int32_t hi) {
    lcg = lcg * 1664525u + 1013904223u;
    return lo + (int32_t)((lcg >> 8) % (uint32_t)(hi - lo + 1));
}

int main(void) {
    // ---- the default is exactly the old motion
    {
        sval_tp_scale_t s = {0};
        for (int i = 0; i < 20000; i++) {
            int32_t x = rnd(-127, 127), y = rnd(-127, 127), ox, oy;
            sval_tp_scale(&s, SVAL_TRACKPOINT_BASE_DPI, x, y, &ox, &oy);
            CHECK(ox == x && oy == y);
            sval_tp_scale(&s, 0, x, y, &ox, &oy); // no speed sent yet
            CHECK(ox == x && oy == y);
        }
        CHECK(s.rem_x == 0 && s.rem_y == 0);
    }

    // ---- twice and half
    {
        sval_tp_scale_t s = {0};
        int32_t         ox, oy;
        sval_tp_scale(&s, 1600, 3, -5, &ox, &oy);
        CHECK(ox == 6 && oy == -10);
        sval_tp_scale(&s, 3200, 3, -5, &ox, &oy);
        CHECK(ox == 12 && oy == -20);
        sval_tp_scale(&s, 400, 4, -6, &ox, &oy);
        CHECK(ox == 2 && oy == -3);
        sval_tp_scale(&s, 200, 4, -4, &ox, &oy);
        CHECK(ox == 1 && oy == -1);
    }

    // ---- remainders carry: nothing is lost at a low speed, the total is exact
    for (unsigned i = 0; i < N; i++) {
        uint16_t        cpi  = choices[i];
        sval_tp_scale_t s    = {0};
        int64_t         in_x = 0, in_y = 0, out_x = 0, out_y = 0;
        for (int k = 0; k < 5000; k++) {
            int32_t x = rnd(-3, 3), y = rnd(-3, 3), ox, oy;
            sval_tp_scale(&s, cpi, x, y, &ox, &oy);
            in_x += x;
            in_y += y;
            out_x += ox;
            out_y += oy;
        }
        // out + remainder/800 == in x cpi/800, exactly
        CHECK(out_x * 800 + s.rem_x == in_x * cpi);
        CHECK(out_y * 800 + s.rem_y == in_y * cpi);
        CHECK(abs(s.rem_x) < 800 && abs(s.rem_y) < 800);
    }

    // ---- a slow push at 200 is not lost: one count in four comes out
    {
        sval_tp_scale_t s     = {0};
        int             total = 0;
        for (int k = 0; k < 100; k++) {
            int32_t ox, oy;
            sval_tp_scale(&s, 200, 1, 0, &ox, &oy);
            total += ox;
        }
        CHECK(total == 25);
    }

    // ---- a negative push moves back the same as a positive one forward
    {
        sval_tp_scale_t a = {0}, b = {0};
        for (int k = 0; k < 1000; k++) {
            int32_t x = rnd(0, 5), ax, ay, bx, by;
            sval_tp_scale(&a, 600, x, x, &ax, &ay);
            sval_tp_scale(&b, 600, -x, -x, &bx, &by);
            CHECK(ax == -bx && ay == -by);
        }
    }

    // ---- a part of a count belongs to the speed it came from
    {
        sval_tp_scale_t s = {0};
        int32_t         ox, oy;
        sval_tp_scale(&s, 200, 1, 1, &ox, &oy); // 1/4 pending
        CHECK(ox == 0 && s.rem_x == 200);
        sval_tp_scale(&s, 400, 0, 0, &ox, &oy); // speed changed: it is dropped
        CHECK(ox == 0 && s.rem_x == 0);
        sval_tp_scale(&s, 200, 1, 1, &ox, &oy);
        sval_tp_scale(&s, 800, 1, 1, &ox, &oy); // back to 1:1: exactly the counts
        CHECK(ox == 1 && oy == 1 && s.rem_x == 0);
    }

    // ---- scroll does not follow the Pointer speed
    // A TrackPoint's scaled counts divide by the speed's DPI (sval_scroll_motion_dpi),
    // so the scroll matches what the raw counts would have given, within the one
    // scaled count the carry still holds.
    for (unsigned i = 0; i < N; i++) {
        uint16_t cpi = choices[i];
        if (cpi > SVAL_DPI_MAX_TRACKPOINT) continue;
        for (int mac = 0; mac < 2; mac++) {
            sval_tp_scale_t    s  = {0};
            sval_ball_scroll_t sr = {0}, ss = {0};
            int64_t            raw = 0, scaled = 0;
            for (int k = 0; k < 4000; k++) {
                int32_t x = rnd(-4, 4), y = rnd(-4, 4), ox, oy;
                int16_t h, v;
                sval_ball_scroll(&sr, x, y, SVAL_BALL_SCROLL_BASE_DPI, SVAL_BALL_SCROLL_BASE_DPI, 100, mac, &h, &v);
                raw += v;
                sval_tp_scale(&s, cpi, x, y, &ox, &oy);
                uint16_t dpi = sval_scroll_motion_dpi(SVAL_SCROLL_KIND_TRACKPOINT, cpi, true);
                sval_ball_scroll(&ss, ox, oy, dpi, dpi, 100, mac, &h, &v);
                scaled += v;
            }
            int64_t tol = 800 / cpi + 2;
            CHECK(raw - scaled <= tol && scaled - raw <= tol);
        }
    }
    // At the default it is the same, step by step.
    {
        sval_tp_scale_t    s  = {0};
        sval_ball_scroll_t sr = {0}, ss = {0};
        for (int k = 0; k < 5000; k++) {
            int32_t x = rnd(-9, 9), y = rnd(-9, 9), ox, oy;
            int16_t h1, v1, h2, v2;
            sval_ball_scroll(&sr, x, y, SVAL_BALL_SCROLL_BASE_DPI, SVAL_BALL_SCROLL_BASE_DPI, 100, false, &h1, &v1);
            sval_tp_scale(&s, 800, x, y, &ox, &oy);
            uint16_t dpi = sval_scroll_motion_dpi(SVAL_SCROLL_KIND_TRACKPOINT, 800, false);
            sval_ball_scroll(&ss, ox, oy, dpi, dpi, 100, false, &h2, &v2);
            CHECK(h1 == h2 && v1 == v2);
        }
    }

    // ---- each module's DPI maximum
    CHECK(sval_dpi_module_max(SVAL_DPI_MODULE_PMW3360) == 12000);
    CHECK(sval_dpi_module_max(SVAL_DPI_MODULE_PMW3389) == 16000);
    CHECK(sval_dpi_module_max(SVAL_DPI_MODULE_TRACKPOINT) == 3200);
    CHECK(choices[sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PMW3360)] == 12000);
    CHECK(choices[sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PMW3389)] == 16000);
    CHECK(choices[sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PAD)] == 1200);
    CHECK(choices[sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_TRACKPOINT)] == 3200);
    CHECK(sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_ANY) == N - 1); // not known yet: the whole list
    CHECK(sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PMW3360) == 10);
    CHECK(sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PMW3389) == 11);
    CHECK(sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_PAD) == 4);
    CHECK(sval_dpi_index_limit(choices, N, SVAL_DPI_MODULE_TRACKPOINT) == 7);

    // The step keys stop there: from 0, increasing one at a time never passes the limit.
    for (uint8_t m = 0; m <= SVAL_DPI_MODULE_TRACKPOINT; m++) {
        uint8_t idx = 0, limit = sval_dpi_index_limit(choices, N, m);
        for (int k = 0; k < 40; k++)
            if (idx < limit) idx++;
        CHECK(idx == limit);
    }

    // A saved index or a write above the maximum is held to it; one within stays.
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PMW3360, 11) == 10); // 16000 on a PMW3360 is 12000
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PMW3360, 10) == 10);
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PMW3389, 11) == 11);
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PAD, 10) == 4);
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PAD, 3) == 3);
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_TRACKPOINT, 9) == 7);
    CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_ANY, 11) == 11);
    for (uint8_t m = 0; m <= SVAL_DPI_MODULE_TRACKPOINT; m++) // a fixed point: a second boot changes nothing
        for (uint8_t i = 0; i < N; i++) {
            uint8_t a = sval_dpi_index_clamp(choices, N, m, i);
            CHECK(sval_dpi_index_clamp(choices, N, m, a) == a);
            CHECK(a <= i);
        }

    // The pad's clamp is the pad normalization the scroll migration already did.
    for (uint8_t i = 0; i < N; i++) CHECK(sval_dpi_index_clamp(choices, N, SVAL_DPI_MODULE_PAD, i) == sval_dpi_index_normalize_pad(choices, N, i));

    // ---- a TrackPoint's list starts at 800
    CHECK(sval_trackpoint_base_index(choices, N) == 3);
    CHECK(choices[sval_trackpoint_base_index(choices, N)] == SVAL_TRACKPOINT_BASE_DPI);
    {
        // Whatever index it had saved (it did nothing), the first boot's reset gives 1:1.
        sval_tp_scale_t s = {0};
        int32_t         ox, oy;
        sval_tp_scale(&s, choices[sval_trackpoint_base_index(choices, N)], 7, -9, &ox, &oy);
        CHECK(ox == 7 && oy == -9);
    }

    if (failures == 0) printf("pointer_speed OK\n");
    return failures ? 1 : 0;
}
