// SPDX-License-Identifier: GPL-2.0-or-later
// Axis Scroll Lock for a trackball hand's scroll (keymaps/keymap_support.c; a
// pad's two-finger scroll has its own, in pad_scroll.c). Not on a Mac.
//
// Motion is in 1/100000 inch (hand_scroll_convert: counts x 100000 / DPI),
// before the Scroll speed. From FREE, motion whose vertical part is at least
// 2/3 of its horizontal part locks the scroll to vertical; otherwise it locks
// to horizontal once the horizontal motion adds up to SVAL_AXIS_LOCK_ENGAGE
// (about 1.6 mm). A locked axis moves to the other one after
// SVAL_AXIS_LOCK_BREAKAWAY (about 4.8 mm) of motion that favors the other
// axis; a report that favors the locked axis starts that count again. Motion
// after a rest of more than SVAL_AXIS_LOCK_REST_MS starts from FREE.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_AXIS_LOCK_BREAKAWAY 18750
#define SVAL_AXIS_LOCK_ENGAGE 6250
#define SVAL_AXIS_LOCK_REST_MS 500
// An axis is favored when 3 x its motion >= 2 x the other axis's.
#define SVAL_AXIS_DOMINANCE_NUMERATOR 3
#define SVAL_AXIS_DOMINANCE_DENOMINATOR 2

enum { SVAL_AXIS_FREE = 0, SVAL_AXIS_LOCKED_V = 1, SVAL_AXIS_LOCKED_H = 2 }; // as POINTING_MOTION (0x2D) reports it

typedef struct {
    uint8_t  mode; // SVAL_AXIS_*
    int32_t  acc_h, acc_v;
    uint32_t last_ms; // the last motion
} sval_axis_lock_t;

// One report's scroll motion. True when the lock moved to the other axis: the
// caller drops the scroll it holds, which belonged to the old axis.
bool sval_axis_lock_step(sval_axis_lock_t *s, uint32_t now_ms, int32_t h, int32_t v);

// The lock now, for POINTING_MOTION (0x2D): FREE once the motion has rested.
uint8_t sval_axis_lock_mode(const sval_axis_lock_t *s, uint32_t now_ms);
