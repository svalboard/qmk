// Wheel pacing (wheel_pace.h).
#include "wheel_pace.h"

static int16_t pace_axis(int32_t *pending, int16_t *phase, int16_t in) {
    // A turn the other way drops what is left of the old direction, and the
    // detent count starts again, as the terminal's does.
    if ((in > 0 && *pending < 0) || (in < 0 && *pending > 0)) *pending = 0;
    if ((in > 0 && *phase < 0) || (in < 0 && *phase > 0)) *phase = 0;
    *pending += in;
    if (*pending > SVAL_WHEEL_PACE_BACKLOG) *pending = SVAL_WHEEL_PACE_BACKLOG;
    if (*pending < -SVAL_WHEEL_PACE_BACKLOG) *pending = -SVAL_WHEEL_PACE_BACKLOG;
    // Up to the next detent boundary, never past it.
    int32_t room = SVAL_WHEEL_PACE_MAX - (*phase < 0 ? -*phase : *phase);
    int32_t out  = *pending;
    if (out > room) out = room;
    if (out < -room) out = -room;
    *pending -= out;
    *phase = (int16_t)((*phase + out) % SVAL_WHEEL_PACE_MAX);
    return (int16_t)out;
}

void sval_wheel_pace(sval_wheel_pace_t *p, int16_t *h, int16_t *v, bool pass) {
    if (pass) {
        *p = (sval_wheel_pace_t){0};
        return;
    }
    *h = pace_axis(&p->h, &p->phase_h, *h);
    *v = pace_axis(&p->v, &p->phase_v, *v);
}
