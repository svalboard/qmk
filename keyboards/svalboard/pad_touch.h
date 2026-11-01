// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The touch page of POINTING_MOTION (Sval 0x2D page 1, docs/protocol.md
// "Touchpad touch"): one pad's touch as the chip's tap and press-and-hold
// rules see it, for the editor's touchpad visualizer. Pure logic, host tested
// in tests/sval_pad_touch; azoteq/touch_stream.c feeds it each pad read and
// carries the other half's block over the split link.
//
// It only watches: nothing here changes what the pad reports.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Events since the block was last packed (byte 1 of a block).
#define SVAL_TOUCH_EV_DOWN 0x01    // a finger touched down
#define SVAL_TOUCH_EV_UP 0x02      // the last finger lifted
#define SVAL_TOUCH_EV_TAP 0x04     // the chip reported a single tap
#define SVAL_TOUCH_EV_TWO_TAP 0x08 // the chip reported a two-finger tap
#define SVAL_TOUCH_EV_HOLD 0x10    // the chip began reporting press and hold
#define SVAL_TOUCH_EV_SCROLL 0x20  // the chip reported a scroll
#define SVAL_TOUCH_EV_DRIFT 0x40   // the touch left the dead zone while a tap or a hold could still fire
#define SVAL_TOUCH_EV_OTHER 0x80   // a swipe or zoom

// Flags (byte 0 of a block).
#define SVAL_TOUCH_VALID 0x01   // this pad is read and its block is live
#define SVAL_TOUCH_DOWN 0x02    // a finger is down now
#define SVAL_TOUCH_OS 0x04      // the computer has this pad as a touchpad: the chip's clicks don't reach it (set by the USB half)
#define SVAL_TOUCH_OUTSIDE 0x08 // this touch has left the dead zone
#define SVAL_TOUCH_FINGERS 0x70 // bits 4-6: fingers down now (0-5)
#define SVAL_TOUCH_HOLDING 0x80 // the chip reports press and hold now

#define SVAL_TOUCH_BLOCK_BYTES 16

// What the chip runs with on this pad.
typedef struct {
    uint16_t xres, yres; // output resolution, pixels across and down the pad
    uint16_t tap_px;     // tap distance, pixels
    uint16_t tap_ms;     // tap time
    uint16_t hold_ms;    // hold time (press and hold fires at tap_ms + hold_ms)
    bool     hold_on;    // press and hold is on
} sval_pad_touch_cfg_t;

typedef struct {
    bool     live, touching, holding, outside;
    uint8_t  fingers, events;
    uint16_t x, y, down_x, down_y; // pixels
    uint16_t farthest;             // 0.01 mm from the down point, this touch
    uint32_t down_at;              // ms
    uint16_t last_ms;              // how long the last touch lasted
    uint16_t strength;             // the first finger's touch strength now (0 with no finger)
    uint8_t  area;                 // and the channels it covers
} sval_pad_touch_t;

// One pad read: fingers and gesture bytes as the chip reports them
// (0x0011, 0x000D, 0x000E), finger 1 at x, y. A failed read changes nothing.
void sval_pad_touch_sample(sval_pad_touch_t *t, const sval_pad_touch_cfg_t *cfg, bool valid, uint8_t fingers, uint8_t ge0, uint8_t ge1, uint16_t x, uint16_t y, uint32_t now_ms);

// The 16-byte block (docs/protocol.md), and clears the events:
//   0 flags, 1 events, 2-3 x, 4-5 y, 6-7 down x, 8-9 down y (0.01 mm, LE),
//   10-11 ms since touch-down (or the last touch's length while up),
//   12-13 farthest from the down point (0.01 mm), 14-15 tap distance (pixels).
void sval_pad_touch_pack(sval_pad_touch_t *t, const sval_pad_touch_cfg_t *cfg, uint32_t now_ms, uint8_t out[SVAL_TOUCH_BLOCK_BYTES]);

// The first finger's size from the same read (0x0016 on: strength, 16-bit,
// and area, the channels it covers); zero while no finger is down.
void sval_pad_touch_size(sval_pad_touch_t *t, uint8_t fingers, uint16_t strength, uint8_t area);
// Its 3 bytes in the touch page: strength (16-bit little-endian), area.
#define SVAL_TOUCH_SIZE_BYTES 3
void sval_pad_touch_pack_size(const sval_pad_touch_t *t, uint8_t out[SVAL_TOUCH_SIZE_BYTES]);

// A packed block's events ORed into another's, for a block held until a host
// reads it (the USB half keeps the other half's this way).
void sval_pad_touch_merge(uint8_t held[SVAL_TOUCH_BLOCK_BYTES], const uint8_t fresh[SVAL_TOUCH_BLOCK_BYTES]);

// ---------------------------------------------------------------- every finger
//
// The fingers page of POINTING_MOTION (Sval 0x2D page 2, docs/protocol.md
// "Touchpad fingers"): every finger the chip reports on one pad, and the
// chip's two-finger rule as it applies it. From the same 45-byte read as the
// block above (register 0x000C on: gesture bytes, system info, the finger
// count, then 7 bytes for each of the 5 finger slots). No I2C of its own.

#define SVAL_FINGERS_MAX 5
#define SVAL_FINGERS_BLOCK_BYTES 27

// Flags (byte 0 of a fingers block).
#define SVAL_FINGERS_VALID 0x01    // this pad is read and its block is live
#define SVAL_FINGERS_OS 0x02       // the computer has this pad as a touchpad (set by the USB half)
#define SVAL_FINGERS_PALM 0x04     // the chip's own palm detect is on now
#define SVAL_FINGERS_TOO_MANY 0x08 // the chip has more fingers than it reports
#define SVAL_FINGERS_TWO 0x10      // a two-finger gesture is live: two fingers, and only two, since the second landed
#define SVAL_FINGERS_SCROLL 0x20   // the chip reported a scroll during that two-finger touch
#define SVAL_FINGERS_TWO_TAP 0x40  // the chip reported a two-finger tap since the block was last packed
#define SVAL_FINGERS_HAD_TWO 0x80  // bytes 22-26 hold a two-finger touch (the live one, or the last)

typedef struct {
    bool     live, two, had_two, scrolled;
    uint8_t  prev_n;               // fingers at the read before
    uint8_t  mask, status, events; // status: SVAL_FINGERS_PALM | SVAL_FINGERS_TOO_MANY now
    uint16_t x[SVAL_FINGERS_MAX], y[SVAL_FINGERS_MAX]; // pixels
    uint8_t  area[SVAL_FINGERS_MAX];
    uint8_t  two_mask;          // the two slots of the two-finger touch
    uint16_t sx[2], sy[2];      // where they were when the second landed, pixels
    uint16_t travel;            // their average travel since, 0.01 mm
    uint32_t two_at;            // ms
    uint16_t two_ms;            // how long it has lasted (or lasted)
} sval_pad_fingers_t;

// One pad read: raw is the 45 bytes from register 0x000C. A failed read changes nothing.
void sval_pad_fingers_sample(sval_pad_fingers_t *f, const sval_pad_touch_cfg_t *cfg, bool valid, const uint8_t *raw, uint32_t now_ms);

// The 27-byte block (docs/protocol.md), and clears the events:
//   0 flags, 1 slots down (bits 0-4), 2-21 five slots of 4 bytes
//   (x | y << 12, 24-bit LE, 0.02 mm; then the area in channels),
//   22-24 the two-finger start centroid (as a slot's position),
//   25 the two fingers' average travel since then (0.05 mm, up to 255),
//   26 ms since the second finger landed, in 4 ms (up to 255).
void sval_pad_fingers_pack(sval_pad_fingers_t *f, const sval_pad_touch_cfg_t *cfg, uint32_t now_ms, uint8_t out[SVAL_FINGERS_BLOCK_BYTES]);

// A packed block's two-finger tap ORed into another's (as sval_pad_touch_merge).
void sval_pad_fingers_merge(uint8_t held[SVAL_FINGERS_BLOCK_BYTES], const uint8_t fresh[SVAL_FINGERS_BLOCK_BYTES]);
