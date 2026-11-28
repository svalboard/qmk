// Host test for axis_lock.c: a trackball hand's Axis Scroll Lock locks to the
// axis the scroll starts on, breaks away after enough motion on the other
// axis, and is free again once the motion has rested. Run by test_detect.py.
#include <stdio.h>
#include "axis_lock.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

// Scroll reports every 10 ms from *t, each moving (h, v); how many switched axis.
static int scroll(sval_axis_lock_t *s, uint32_t *t, int reports, int32_t h, int32_t v) {
    int switched = 0;
    for (int i = 0; i < reports; i++) {
        *t += 10;
        switched += sval_axis_lock_step(s, *t, h, v);
    }
    return switched;
}

int main(void) {
    sval_axis_lock_t s = {0};
    uint32_t         t = 1000;

    // A vertical scroll locks vertical at once.
    CHECK(scroll(&s, &t, 1, 0, 400) == 0);
    CHECK(s.mode == SVAL_AXIS_LOCKED_V);
    // Horizontal motion right after it breaks away only after the breakaway distance.
    CHECK(scroll(&s, &t, SVAL_AXIS_LOCK_BREAKAWAY / 1000 - 1, 1000, 0) == 0);
    CHECK(s.mode == SVAL_AXIS_LOCKED_V);
    CHECK(scroll(&s, &t, 2, 1000, 0) == 1);
    CHECK(s.mode == SVAL_AXIS_LOCKED_H);

    // A pause: a horizontal scroll a while after a vertical one starts free,
    // and locks horizontal after the engage distance, not the breakaway.
    sval_axis_lock_t p = {0};
    t                  = 5000;
    CHECK(scroll(&p, &t, 5, 0, 400) == 0);
    CHECK(p.mode == SVAL_AXIS_LOCKED_V);
    CHECK(sval_axis_lock_mode(&p, t + SVAL_AXIS_LOCK_REST_MS) == SVAL_AXIS_LOCKED_V);
    t += SVAL_AXIS_LOCK_REST_MS + 1;
    CHECK(sval_axis_lock_mode(&p, t) == SVAL_AXIS_FREE);                   // 0x2D reports it free during the rest
    CHECK(scroll(&p, &t, SVAL_AXIS_LOCK_ENGAGE / 1000 + 1, 1000, 0) == 0); // from FREE: no switch to drop
    CHECK(p.mode == SVAL_AXIS_LOCKED_H);
    // ... and the other way round.
    t += SVAL_AXIS_LOCK_REST_MS + 1;
    CHECK(scroll(&p, &t, 1, 0, 400) == 0);
    CHECK(p.mode == SVAL_AXIS_LOCKED_V);

    // A pause under the rest keeps the lock.
    t += SVAL_AXIS_LOCK_REST_MS - 50;
    CHECK(scroll(&p, &t, 3, 1000, 0) == 0);
    CHECK(p.mode == SVAL_AXIS_LOCKED_V);

    // The rest is counted on a 32-bit clock: a pause near a multiple of 65.536 s still frees it.
    sval_axis_lock_t w = {0};
    t                  = 70000;
    CHECK(scroll(&w, &t, 1, 0, 400) == 0);
    CHECK(w.mode == SVAL_AXIS_LOCKED_V);
    t += 65536 + 100;
    CHECK(scroll(&w, &t, SVAL_AXIS_LOCK_ENGAGE / 1000 + 1, 1000, 0) == 0);
    CHECK(w.mode == SVAL_AXIS_LOCKED_H);

    if (failures == 0) printf("axis_lock OK\n");
    return failures != 0;
}
