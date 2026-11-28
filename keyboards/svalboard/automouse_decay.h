// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Auto Mouse's decay (keymaps/keymap_support.c): motion below the threshold
// is forgotten when the next motion comes more than the Activation Decay
// later.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Whether the partial count has decayed: more than decay_ms since the motion
// that last added to it, both times from timer_read32(). A 16-bit time wrapped
// every 65.5 s, so motion that came an idle of about a multiple of that later
// still found the old count. 32 bits wrap only after 49 days.
static inline bool sval_automouse_decayed(uint32_t now, uint32_t last, uint16_t decay_ms) {
    return decay_ms > 0 && (uint32_t)(now - last) > decay_ms;
}
