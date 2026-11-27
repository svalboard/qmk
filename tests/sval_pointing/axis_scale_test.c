// add_to_axis (keyboards/svalboard/axis_scale.c): Boost saturates instead of
// wrapping the int16 remainder (F26), and each hand keeps its own Sniper
// remainders (F25: keymaps/keymap_support.c keeps one axis_scale_set_t per hand).
#include <stdio.h>
#include "axis_scale.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void) {
    // ---- Boost: a fast flick at the largest factor stops at full speed, never reverses
    {
        axis_scale_t b = {1, 1, 0};
        set_mult_axis(&b, 255);
        CHECK(add_to_axis(&b, 10) == 2550);
        CHECK(add_to_axis(&b, 200) == 32767); // 51000 wrapped to -14536
        CHECK(b.remainder == 0);
        CHECK(add_to_axis(&b, -200) == -32768);
        CHECK(add_to_axis(&b, 128) == 32640);
        CHECK(add_to_axis(&b, 129) == 32767); // 32895
        for (int v = -32768; v <= 32767; v += 7) {
            int16_t out = add_to_axis(&b, (int16_t)v);
            CHECK((v > 0 && out > 0) || (v < 0 && out < 0) || (v == 0 && out == 0));
        }
    }
    // ---- Sniper as before: a divisor carries the part of a count to the next report
    {
        axis_scale_t s = {1, 1, 0};
        set_div_axis(&s, 3);
        int total = 0;
        for (int i = 0; i < 9; i++)
            total += add_to_axis(&s, 1);
        CHECK(total == 3);
        CHECK(add_to_axis(&s, -32768) == -10922 && s.remainder == -2);
        set_div_axis(&s, 2);
        CHECK(add_to_axis(&s, 32767) == 16383 && s.remainder == 1);
        CHECK(add_to_axis(&s, 1) == 1 && s.remainder == 0);
    }
    // ---- Sniper per hand: one hand's leftover fraction never moves the other hand
    {
        axis_scale_set_t sniper[2] = {AXIS_SCALE_SET_INIT, AXIS_SCALE_SET_INIT};
        set_div_axes(&sniper[0], 2);
        set_div_axes(&sniper[1], 2);
        // One count from each hand, report after report: each hand moves every other report.
        CHECK(add_to_axis(&sniper[0].x, 1) == 0);
        CHECK(add_to_axis(&sniper[1].x, 1) == 0); // one shared remainder gave this hand the left hand's half count
        CHECK(add_to_axis(&sniper[0].x, 1) == 1);
        CHECK(add_to_axis(&sniper[1].x, 1) == 1);
        // Opposite motion on the two hands no longer cancels.
        CHECK(add_to_axis(&sniper[0].y, 1) == 0);
        CHECK(add_to_axis(&sniper[1].y, -1) == 0);
        CHECK(add_to_axis(&sniper[0].y, 1) == 1);
        CHECK(add_to_axis(&sniper[1].y, -1) == -1);
        // The four axes are separate, and a new divisor clears every remainder.
        CHECK(add_to_axis(&sniper[1].h, 1) == 0 && sniper[1].v.remainder == 0);
        set_div_axes(&sniper[1], 4);
        CHECK(sniper[1].h.remainder == 0 && sniper[1].x.div == 4 && sniper[1].v.div == 4 && sniper[0].x.div == 2);
    }
    if (failures) return 1;
    printf("axis_scale OK\n");
    return 0;
}
