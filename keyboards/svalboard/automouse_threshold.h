// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Auto Mouse threshold: how far the pointer moves before the auto mouse
// layer turns on. Mouse motion (trackballs, sensors) and native touchpad
// contact motion add into one accumulator, so both are counted in the mouse
// path's unit: 1/SVAL_AUTOMOUSE_FP_SCALE of a count at the 800 DPI
// reference. The threshold setting (automouse_threshold) is in whole 800 DPI
// counts.
//
// Activation Decay (automouse_decay, in 10 ms steps): the accumulator starts
// over after a pause in motion longer than this. It is at least 10 ms; a
// stored 0 (which once meant "never") reads as the default, 70 ms.
//
// Pure functions: no QMK headers, so the host tests build them as they are.

#include <stdbool.h>
#include <stdint.h>
#include "automouse_decay.h"

#define SVAL_AUTOMOUSE_REF_DPI 800
#define SVAL_AUTOMOUSE_FP_SCALE 16
#define SVAL_AUTOMOUSE_DECAY_DEFAULT 7 // 70 ms

// Motion of `counts` (|x| + |y|) at `per_inch` counts per inch, in
// accumulator units. Fixed point, so motion under one reference count
// (resolution above 800) is kept. Mice pass their DPI; the touchpad passes
// its counts per inch.
static inline int32_t sval_automouse_units(int32_t counts, int32_t per_inch) {
    if (counts <= 0 || per_inch <= 0) return 0;
    return (counts * SVAL_AUTOMOUSE_REF_DPI * SVAL_AUTOMOUSE_FP_SCALE) / per_inch;
}

// The Activation Decay setting as used: a stored 0 reads as the default.
static inline uint8_t sval_automouse_decay_setting(uint8_t stored) {
    return stored ? stored : SVAL_AUTOMOUSE_DECAY_DEFAULT;
}

// Activation Decay in milliseconds: 10 at the least, never "no decay".
static inline uint16_t sval_automouse_decay_ms(uint8_t stored) {
    return (uint16_t)sval_automouse_decay_setting(stored) * 10;
}

// Adds units of motion at now_ms. The accumulator starts over when more than
// the Activation Decay passed since the last motion. Returns true when the
// layer should turn on: threshold 0 turns it on at any motion. Times are
// timer_read32() (automouse_decay.h: a 16-bit time wrapped every 65.5 s).
static inline bool sval_automouse_accumulate(int32_t *accumulator, uint32_t *last_ms, uint32_t now_ms, uint8_t decay_stored, int32_t units, uint16_t threshold) {
    if (sval_automouse_decayed(now_ms, *last_ms, sval_automouse_decay_ms(decay_stored))) *accumulator = 0;
    *last_ms = now_ms;
    *accumulator += units;
    return threshold == 0 || *accumulator >= (int32_t)threshold * SVAL_AUTOMOUSE_FP_SCALE;
}
