// Pure helpers for the pointing fragments (Sval 0x18) and POINTING_MOTION (Sval 0x2D),
// kept free of QMK headers so tests/sval_pointing can build them on the host.
// Layout of the 0x2D reply: docs/protocol.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pointing_motion_vs.h"

#define SVAL_MOTION_FORMAT     1
#define SVAL_MOTION_REPORT_LEN 24 // bytes after [0xDF] [0x2D]
#define SVAL_MOTION_HAND_LEN   10
#define SVAL_MOTION_LEFT_OFF   3
#define SVAL_MOTION_RIGHT_OFF  13
#define SVAL_MOTION_AXIS_OFF   23
// The sniper and boost factors in force, after the Velocity Scroll block:
// magic, then the sniper divisor and the boost multiplier (1 when that key is
// off; 255 at most). Firmware without them leaves the magic 0.
#define SVAL_MOTION_FACTORS_OFF   53
#define SVAL_MOTION_FACTORS_LEN   3
#define SVAL_MOTION_FACTORS_MAGIC 0x46 // 'F'


// Global mode bits (reply byte 1).
#define SVAL_MOTION_SNIPER         0x01 // sniper / precision active
#define SVAL_MOTION_BOOST          0x02 // boost active
#define SVAL_MOTION_AUTOMOUSE_ON   0x04 // the auto-mouse layer is on
#define SVAL_MOTION_AXIS_LOCK      0x08 // axis scroll lock is in effect
#define SVAL_MOTION_NATURAL_SCROLL 0x10
#define SVAL_MOTION_SCROLL_KEY     0x20 // scroll hold or scroll toggle key active

// Per-hand flags (block byte 0).
#define SVAL_MOTION_VALID         0x01
#define SVAL_MOTION_SCROLLING     0x02
#define SVAL_MOTION_SATURATED     0x04
#define SVAL_MOTION_AUTOMOUSE     0x08 // auto-mouse is enabled for this hand
#define SVAL_MOTION_TOUCH_VALID   0x10
#define SVAL_MOTION_FINGERS_SHIFT 5 // bits 5-6, 0..3

// One hand's motion since the last 0x2D read.
typedef struct {
    int32_t x, y;
    int16_t h, v;
    uint8_t buttons;
    bool    saturated;
} sval_motion_acc_t;

static inline int32_t sval_motion_add32(int32_t a, int32_t b, bool *sat) {
    if (b > 0 && a > INT32_MAX - b) { *sat = true; return INT32_MAX; }
    if (b < 0 && a < INT32_MIN - b) { *sat = true; return INT32_MIN; }
    return a + b;
}

static inline int32_t sval_motion_clamp(int32_t v, int32_t lo, int32_t hi, bool *sat) {
    if (v > hi) { *sat = true; return hi; }
    if (v < lo) { *sat = true; return lo; }
    return v;
}

static inline void sval_motion_accumulate(sval_motion_acc_t *acc, int32_t x, int32_t y, int32_t h, int32_t v, uint8_t buttons) {
    acc->x = sval_motion_add32(acc->x, x, &acc->saturated);
    acc->y = sval_motion_add32(acc->y, y, &acc->saturated);
    acc->h = (int16_t)sval_motion_clamp((int32_t)acc->h + h, INT16_MIN, INT16_MAX, &acc->saturated);
    acc->v = (int16_t)sval_motion_clamp((int32_t)acc->v + v, INT16_MIN, INT16_MAX, &acc->saturated);
    acc->buttons |= buttons;
}

// Writes one 10-byte hand block. flags carries VALID/SCROLLING/AUTOMOUSE/touch bits;
// SATURATED is added when anything was clamped. The caller clears acc afterwards.
static inline void sval_motion_pack_hand(uint8_t *out, const sval_motion_acc_t *acc, uint8_t flags) {
    bool    sat = acc->saturated;
    int16_t dx  = (int16_t)sval_motion_clamp(acc->x, INT16_MIN, INT16_MAX, &sat);
    int16_t dy  = (int16_t)sval_motion_clamp(acc->y, INT16_MIN, INT16_MAX, &sat);
    int8_t  h   = (int8_t)sval_motion_clamp(acc->h, INT8_MIN, INT8_MAX, &sat);
    int8_t  v   = (int8_t)sval_motion_clamp(acc->v, INT8_MIN, INT8_MAX, &sat);
    if (sat) flags |= SVAL_MOTION_SATURATED;
    out[0] = flags & 0x7F;
    out[1] = (uint8_t)((uint16_t)dx & 0xFF);
    out[2] = (uint8_t)((uint16_t)dx >> 8);
    out[3] = (uint8_t)((uint16_t)dy & 0xFF);
    out[4] = (uint8_t)((uint16_t)dy >> 8);
    out[5] = (uint8_t)h;
    out[6] = (uint8_t)v;
    out[7] = acc->buttons;
    out[8] = 0; // touch x: positions are not reported yet
    out[9] = 0; // touch y
}

// Elapsed ms for reply byte 2: 0 on the first read, else saturating at 255.
static inline uint8_t sval_motion_elapsed(bool first_read, uint32_t elapsed_ms) {
    if (first_read) return 0;
    return elapsed_ms > 255 ? 255 : (uint8_t)elapsed_ms;
}

// The pointing fragment for a POINTING_HARDWARE `detected` code (enum sval_pointer_sensor),
// or 0xFF when nothing is known yet. Needs the generated SVAL_FRAGMENT_ID_POINTING_* names.
#ifdef SVAL_FRAGMENT_ID_POINTING_NONE
static inline uint8_t sval_pointing_fragment_for_detected(uint8_t detected) {
    switch (detected) {
        case 1: return SVAL_FRAGMENT_ID_POINTING_TRACKBALL_PMW3360; // SVAL_SENSOR_PMW3360
        case 2: return SVAL_FRAGMENT_ID_POINTING_TRACKBALL_PMW3389; // SVAL_SENSOR_PMW3389
        case 3: return SVAL_FRAGMENT_ID_POINTING_NONE;              // SVAL_SENSOR_NO_RESPONSE
        case 4: return SVAL_FRAGMENT_ID_POINTING_UNKNOWN;           // SVAL_SENSOR_UNKNOWN
        case 5: return SVAL_FRAGMENT_ID_POINTING_TOUCHPAD_AZOTEQ;   // SVAL_SENSOR_AZOTEQ
        case 6: return SVAL_FRAGMENT_ID_POINTING_TRACKPOINT;        // SVAL_SENSOR_TRACKPOINT
        default: return 0xFF;                                       // 0: not checked yet
    }
}
#endif
