// SPDX-License-Identifier: GPL-2.0-or-later
// Velocity Scroll (SV_VELOCITY_SCROLL): hold the key, and how far the pointer
// has moved from where it was when the key went down sets a scroll speed. The
// board keeps scrolling at that speed while the pointer stays still, like
// middle-click autoscroll on a desktop, but in the firmware, so it works in
// every app. SV_VELOCITY_SCROLL_TOGGLE is the same without holding: a tap
// turns it on, anchored where the pointer is then, and the next tap turns it
// off. Below, "while the key is held" means while it is on either way, and
// "key-down" the moment it turned on.
//
// The math lives here, free of QMK headers, so tests/sval_pointing builds it
// on the host. keymaps/keymap_support.c feeds it and sends what it returns.
//
// Design
// - The cursor is frozen while the key is held: no x/y reaches the host, and
//   no other scroll (Drag Scroll, the pads' own gestures) either. Buttons pass.
// - Trackball (and any relative pointer): its counts since key-down add up to
//   a virtual offset, like a joystick. Rolling back toward the anchor slows the
//   scroll and stops it there. Each axis of the offset is clamped where the
//   speed reaches its maximum, so rolling on never winds up a debt that must be
//   unrolled before the speed drops.
// - Touchpad (native touchpad builds): the firmware takes the pad from the
//   operating system while the key is held (ptp.c, sval_ptp_set_grab) and reads
//   the finger's absolute position. The offset is the finger's position minus
//   where it was at the first sample after key-down, or where it touched down
//   if it landed later. Lifting the finger stops the scroll; touching again
//   anchors again. With both pads, the pad touched is the one used; a finger
//   resting on the other pad never takes over, and the pads are never merged.
// - Distance is physical (mm of ball surface or of pad), so DPI never changes
//   the feel: the same normalization the auto-mouse threshold does with its
//   800-DPI reference, taken to millimeters.
// - Speed, per axis: a dead zone, then speed = gain * (past / 10 mm) ^ exponent
//   notches per second, capped at the maximum. Per axis means a small sideways
//   drift inside the dead zone never scrolls sideways. Axis lock (a tunable)
//   lets only the axis that left its dead zone first scroll, until both are
//   back inside.
// - Output is integrated over real elapsed time and sent on a timer, whether
//   the pointer moves or not. Units are high-resolution wheel units (120 a
//   notch) when the descriptor has the resolution multiplier, else whole
//   notches; fractions carry, so slow speeds stay smooth.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// ------------------------------------------------------------------ tunables
// Build-time defaults. Candidates for Keybard settings later: the two gains,
// the maximum speed and the axis lock. The rest are for tuning only.

// Trackball: dead zone, gain (notches/s at 10 mm past the dead zone), exponent.
#ifndef SVAL_VS_BALL_DEAD_MM
#    define SVAL_VS_BALL_DEAD_MM 0.8f
#endif
#ifndef SVAL_VS_BALL_GAIN
#    define SVAL_VS_BALL_GAIN 12.0f
#endif
#ifndef SVAL_VS_BALL_EXPONENT
#    define SVAL_VS_BALL_EXPONENT 1.5f
#endif
// Touchpad: a finger rolls and jitters more than a ball, and the pad is only
// 43 x 40 mm, so a larger dead zone and a steeper start.
#ifndef SVAL_VS_PAD_DEAD_MM
#    define SVAL_VS_PAD_DEAD_MM 1.5f
#endif
#ifndef SVAL_VS_PAD_GAIN
#    define SVAL_VS_PAD_GAIN 16.0f
#endif
#ifndef SVAL_VS_PAD_EXPONENT
#    define SVAL_VS_PAD_EXPONENT 1.5f
#endif
// Both: the speed cap, notches per second.
#ifndef SVAL_VS_MAX_NPS
#    define SVAL_VS_MAX_NPS 60.0f
#endif
// 0 both axes scroll at once; 1 only the axis that left its dead zone first.
#ifndef SVAL_VS_AXIS_LOCK
#    define SVAL_VS_AXIS_LOCK 0
#endif
// How often a scroll report goes out while the speed is not zero.
#ifndef SVAL_VS_PERIOD_MS
#    define SVAL_VS_PERIOD_MS 8
#endif
// Trackball offset clamp, per axis: this far past the point of maximum speed
// (0: exactly there). Negative turns the clamp off.
#ifndef SVAL_VS_BALL_CLAMP_MARGIN_MM
#    define SVAL_VS_BALL_CLAMP_MARGIN_MM 0.0f
#endif

// The pads' size (the TPS43 active area), for counts to millimeters.
#define SVAL_VS_PAD_MM_X 43.0f
#define SVAL_VS_PAD_MM_Y 40.0f

// ------------------------------------------------------------------ curve

typedef struct {
    float dead_mm;  // per axis
    float gain;     // notches/s at 10 mm past the dead zone
    float exponent; // > 0
    float max_nps;  // cap, notches/s
} sval_vs_curve_t;

// Signed speed in notches/s for an offset along one axis, in mm.
float sval_vs_speed(const sval_vs_curve_t *c, float offset_mm);
// The offset where the speed first reaches max_nps (the clamp point).
float sval_vs_full_mm(const sval_vs_curve_t *c);

// ------------------------------------------------------------------ state

#define SVAL_VS_NO_PAD 0xFF

typedef struct {
    bool     active;
    float    rel_x, rel_y; // relative pointers' offset, mm
    float    frac_h, frac_v; // output units not yet sent
    uint8_t  lock;         // axis lock: 0 none yet, 1 vertical, 2 horizontal
    // Touchpad: the pad whose finger is in use, its touch and anchor.
    uint8_t  pad;          // 0 left, 1 right, SVAL_VS_NO_PAD
    uint16_t touch;        // that finger's touch serial (ptp.c)
    uint16_t accept_after; // only touches with a newer serial are taken
    uint16_t newest;       // the newest serial seen
    float    anchor_x, anchor_y, pad_x, pad_y; // mm
    float    speed_h, speed_v; // the speed the last step sent at, notches/s (+right, +up)
} sval_vs_t;

void sval_vs_start(sval_vs_t *s);
void sval_vs_stop(sval_vs_t *s);

// ------------------------------------------------------------------ keys
// What keeps it on: SV_VELOCITY_SCROLL keys held, the toggle
// (SV_VELOCITY_SCROLL_TOGGLE) and the dev builds' keyless hold. It is on while
// any of them is. Only a change from off to on starts it, with a new anchor,
// so pressing and releasing the hold key while the toggle is on changes
// nothing.
typedef struct {
    uint8_t keys;    // SV_VELOCITY_SCROLL keys down
    bool    toggled; // SV_VELOCITY_SCROLL_TOGGLE: flipped by each press
    bool    dev;     // velocity_scroll_dev_hold (dev builds)
} sval_vs_hold_t;

bool sval_vs_hold_on(const sval_vs_hold_t *h);
// A Velocity Scroll key went down or up; toggle: it is the toggle key.
void sval_vs_key(sval_vs_hold_t *h, bool toggle, bool pressed);
// Bring s in line with h: start it when h turns on, stop it when h turns off.
// Returns 1 started, -1 stopped, 0 unchanged.
int8_t sval_vs_apply(sval_vs_t *s, const sval_vs_hold_t *h);

// A relative pointer moved this many mm (+x right, +y toward the user).
// clamp_mm < 0: no clamp.
void sval_vs_add_relative(sval_vs_t *s, float dx_mm, float dy_mm, float clamp_mm);

// One pad's finger this sample: touch 0 none, else its serial; x, y in mm on
// the pad. Call for each pad with a finger tracker, every update.
void sval_vs_pad(sval_vs_t *s, uint8_t pad, uint16_t touch, float x_mm, float y_mm);
// A finger is in use on a pad.
bool sval_vs_touching(const sval_vs_t *s);
// The offset that sets the speed now, mm (+x right, +y toward the user): the
// finger's from its anchor while one is in use, else the relative pointers'.
void sval_vs_offset(const sval_vs_t *s, float *x_mm, float *y_mm);

// Wheel units per notch. Windows and Linux apply the wheel's resolution
// multiplier (120 counts a notch, 3 lines on Windows), or count a notch per
// unit without one. macOS ignores the multiplier and scrolls a line per count,
// so a notch there is 3 counts: the lines Windows scrolls for the same speed,
// as Scroll Mode's 40 counts per line (SVAL_BALL_SCROLL_MAC_DIV in
// scroll_speed.h). It was 120 / 12 (MAC_DIVISOR), 10 lines a notch, through
// x4.6.7: about 3.3x.
#define SVAL_VS_MAC_UNITS_PER_NOTCH 3
// hires: the multiplier (0 without the high-resolution wheel).
float sval_vs_units_per_notch(uint16_t hires, bool mac);

// Advance dt_ms and return the wheel units to send now, units_per_notch from
// sval_vs_units_per_notch. h > 0 scrolls right, v > 0 scrolls up (away),
// before natural scroll, which the caller applies with the rest of the report.
void sval_vs_step(sval_vs_t *s, const sval_vs_curve_t *ball, const sval_vs_curve_t *pad, bool axis_lock, float dt_ms, float units_per_notch, int16_t *h, int16_t *v);
