// axis_factor (keyboards/svalboard/axis_scale.h): the sniper divisor and boost
// multiplier from the keys on, clamped to 255 and never wrapped.
#include <stdio.h>
#include "axis_scale.h"

#define CHECK(c)                                        \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                   \
        }                                               \
    } while (0)

int main(void) {
    CHECK(axis_factor(0, 0, 0) == 1);
    CHECK(axis_factor(1, 0, 0) == 2 && axis_factor(0, 1, 0) == 3 && axis_factor(0, 0, 1) == 5);
    CHECK(axis_factor(1, 1, 1) == 30);
    CHECK(axis_factor(7, 0, 0) == 128);
    CHECK(axis_factor(8, 0, 0) == 255); // 256 used to wrap to 0 (no sniper at all)
    CHECK(axis_factor(9, 0, 0) == 255); // 512 used to wrap to 0
    CHECK(axis_factor(0, 5, 0) == 243 && axis_factor(0, 6, 0) == 255);
    CHECK(axis_factor(2, 2, 2) == 255); // 900
    CHECK(axis_factor(40, 40, 40) == 255);
    for (unsigned a = 0; a < 12; a++)
        for (unsigned b = 0; b < 8; b++)
            for (unsigned c = 0; c < 6; c++)
                CHECK(axis_factor(a, b, c) >= 1);
    printf("OK\n");
    return 0;
}
