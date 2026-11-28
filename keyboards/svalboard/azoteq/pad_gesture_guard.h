// SPDX-License-Identifier: GPL-2.0-or-later
// Which of the IQS5xx's own gesture reports reach the mouse (azoteq.c). Pure
// logic, host-tested in tests/sval_native_ptp/test_gesture_guard.c.
//
// The chip's base data (IQS5xx-B000 datasheet 8.9, from 0x000C) is ten bytes:
// previous cycle time, gesture events 0 and 1, system info 0 and 1, the
// number of fingers, then relative X and Y. A gesture bit is set for one
// cycle, in the read after the gesture ended (a tap: after the fingers lift).
//
// Phantom right clicks (ticket 0729, macOS, mouse path): single reads, seconds
// after the last touch, with gesture events 1 = 0x03 (two-finger tap and
// scroll at once) and no fingers. Two mutually exclusive gestures, a tap with
// no touch before it, and exactly the bytes a read one register late would
// give while the chip is in LP1 (system info 0, CHARGING_MODE = 011, in the
// gesture events 1 position; datasheet 8.10.3). So a report is believed only
// when it is self-consistent and the fingers it needs were seen just before.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PAD_GUARD_BASE_BYTES 10

// Base data offsets.
#define PAD_B_CYCLE 0
#define PAD_B_GE0 1
#define PAD_B_GE1 2
#define PAD_B_SYS0 3
#define PAD_B_SYS1 4
#define PAD_B_FINGERS 5

// Gesture events 0 and 1 (datasheet 8.10.1, 8.10.2).
#define PAD_GE0_TAP 0x01
#define PAD_GE0_HOLD 0x02
#define PAD_GE0_SWIPES 0x3C
#define PAD_GE0_UNUSED 0xC0
#define PAD_GE1_TWO_TAP 0x01
#define PAD_GE1_SCROLL 0x02
#define PAD_GE1_ZOOM 0x04
#define PAD_GE1_UNUSED 0xF8
#define PAD_SYS1_UNUSED 0xC0
#define PAD_MAX_FINGERS 5

// How long after the last frame with the fingers a gesture needs it still
// counts, beyond the tap time: a tap is reported in the read after the lift,
// one chip cycle (10 ms active) later, so this is margin for missed reads.
#ifndef PAD_GUARD_MARGIN_MS
#    define PAD_GUARD_MARGIN_MS 250
#endif

// Why a report was not believed (0: it was, perhaps with some bits cleared
// by a setting). Only the reasons that need a phantom are logged as one.
enum {
    PAD_GUARD_OK          = 0,
    PAD_GUARD_BAD_FRAME   = 1, // a byte no real frame holds: the whole read is dropped
    PAD_GUARD_CONFLICT    = 2, // two-finger tap with scroll or zoom in the same report
    PAD_GUARD_NO_TWO      = 3, // two-finger tap with no 2-finger frame just before
    PAD_GUARD_NO_TOUCH    = 4, // tap, hold or swipe with no touch just before
    PAD_GUARD_WOKE        = 5, // gesture in a read the chip answered from low power
};

// What the keyboard allows (the Keybard touchpad settings and the build).
typedef struct {
    bool     tap, two_finger, hold, swipes;
    uint16_t tap_ms; // the chip's tap time now (sval_pad_tap_time_ms)
} pad_guard_cfg_t;

typedef struct {
    uint32_t one_at, two_at; // last frame with at least 1 / at least 2 fingers (ms)
    bool     one_seen, two_seen;
} pad_guard_t;

static inline void pad_guard_init(pad_guard_t *g) {
    g->one_at = g->two_at = 0;
    g->one_seen = g->two_seen = false;
}

// True when no real frame could hold these bytes. A cycle time of 0 is
// impossible (the chip measures its own last cycle, at least 1 ms), and is
// what a read one register late shows: gesture events 0 in its place.
static inline bool pad_guard_bad_frame(const uint8_t b[PAD_GUARD_BASE_BYTES]) {
    return b[PAD_B_CYCLE] == 0 || (b[PAD_B_GE0] & PAD_GE0_UNUSED) || (b[PAD_B_GE1] & PAD_GE1_UNUSED) || (b[PAD_B_SYS1] & PAD_SYS1_UNUSED) || b[PAD_B_FINGERS] > PAD_MAX_FINGERS;
}

static inline bool pad_guard_recent(bool seen, uint32_t at, uint32_t now, uint32_t window) {
    return seen && (uint32_t)(now - at) <= window;
}

// One successful read at now_ms. woke: the chip NACKed the first addressing
// (low power, datasheet 8.8.2) and the read was retried. Clears, in b, every
// gesture bit that is not believed or not allowed, and returns why the first
// believed-phantom bit was cleared (0 if none was). Motion is left alone,
// except that a bad frame is reported so the caller drops all of it.
static inline uint8_t pad_guard_frame(pad_guard_t *g, const pad_guard_cfg_t *cfg, uint8_t b[PAD_GUARD_BASE_BYTES], uint32_t now_ms, bool woke) {
    if (pad_guard_bad_frame(b)) return PAD_GUARD_BAD_FRAME;
    const uint32_t window = (uint32_t)cfg->tap_ms + PAD_GUARD_MARGIN_MS;
    uint8_t        ge0 = b[PAD_B_GE0], ge1 = b[PAD_B_GE1], why = PAD_GUARD_OK;
    const uint8_t  fingers = b[PAD_B_FINGERS];

    // The fingers this frame shows count too (a hold is reported while down).
    if (fingers >= 1) g->one_at = now_ms, g->one_seen = true;
    if (fingers >= 2) g->two_at = now_ms, g->two_seen = true;

    if ((ge0 | (ge1 & (PAD_GE1_TWO_TAP | PAD_GE1_ZOOM))) && woke) {
        // A gesture needs the chip awake (Active): one that just woke it from
        // low power has no touch behind it. Scroll keeps its movement.
        ge0 = 0;
        ge1 &= (uint8_t)~(PAD_GE1_TWO_TAP | PAD_GE1_ZOOM);
        why = PAD_GUARD_WOKE;
    }
    if ((ge1 & PAD_GE1_TWO_TAP) && (ge1 & (PAD_GE1_SCROLL | PAD_GE1_ZOOM))) {
        // Exclusive gestures in one report: believe none of gesture events 1.
        ge1 = 0;
        if (!why) why = PAD_GUARD_CONFLICT;
    }
    if ((ge1 & PAD_GE1_TWO_TAP) && !pad_guard_recent(g->two_seen, g->two_at, now_ms, window)) {
        ge1 &= (uint8_t)~PAD_GE1_TWO_TAP;
        if (!why) why = PAD_GUARD_NO_TWO;
    }
    if ((ge0 & (PAD_GE0_TAP | PAD_GE0_HOLD | PAD_GE0_SWIPES)) && !pad_guard_recent(g->one_seen, g->one_at, now_ms, window)) {
        ge0 = 0;
        if (!why) why = PAD_GUARD_NO_TOUCH;
    }
    // The settings: off means no click, whatever the chip's enable bit says.
    if (!cfg->tap) ge0 &= (uint8_t)~PAD_GE0_TAP;
    if (!cfg->hold) ge0 &= (uint8_t)~PAD_GE0_HOLD;
    if (!cfg->swipes) ge0 &= (uint8_t)~PAD_GE0_SWIPES;
    if (!cfg->two_finger) ge1 &= (uint8_t)~PAD_GE1_TWO_TAP;

    b[PAD_B_GE0] = ge0;
    b[PAD_B_GE1] = ge1;
    return why;
}
