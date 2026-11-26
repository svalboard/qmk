// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Auto Mouse threshold: how far the pointer moves before the auto mouse
// layer turns on. Mouse motion (trackballs, sensors) and native touchpad
// contact motion add into one accumulator, so both are counted in one unit:
// 1/SVAL_AUTOMOUSE_FP_SCALE of a count at the 800 DPI reference. The
// threshold setting (automouse_threshold) is in whole 800 DPI counts.
//
// Pure functions: no QMK headers, so the host tests build them as they are.

#include <stdbool.h>
#include <stdint.h>

#define SVAL_AUTOMOUSE_REF_DPI 800
#define SVAL_AUTOMOUSE_FP_SCALE 16

// Mouse motion: |x| + |y| counts at the hand's DPI, in accumulator units.
// Fixed point, so motion under one reference count (DPI above 800) is kept.
static inline int32_t sval_automouse_mouse_units(int32_t counts, uint16_t dpi) {
    if (counts <= 0 || dpi == 0) return 0;
    return (counts * SVAL_AUTOMOUSE_REF_DPI * SVAL_AUTOMOUSE_FP_SCALE) / dpi;
}

// Touchpad contact motion: counts at counts_per_inch, in accumulator units.
// Truncated to whole reference counts first, as the pad path always has, so a
// pad alone reaches the threshold at exactly the same motion as before.
static inline int32_t sval_automouse_pad_units(int32_t counts, int32_t counts_per_inch) {
    if (counts <= 0 || counts_per_inch <= 0) return 0;
    return ((counts * SVAL_AUTOMOUSE_REF_DPI) / counts_per_inch) * SVAL_AUTOMOUSE_FP_SCALE;
}

// Adds units of motion at now_ms. The accumulator starts over when more than
// decay_ms passed since the last motion (decay_ms 0: never). Returns true when
// the layer should turn on: threshold 0 turns it on at any motion.
static inline bool sval_automouse_accumulate(int32_t *accumulator, uint16_t *last_ms, uint16_t now_ms, uint16_t decay_ms, int32_t units, uint16_t threshold) {
    if (decay_ms > 0 && (uint16_t)(now_ms - *last_ms) > decay_ms) *accumulator = 0;
    *last_ms = now_ms;
    *accumulator += units;
    return threshold == 0 || *accumulator >= (int32_t)threshold * SVAL_AUTOMOUSE_FP_SCALE;
}
