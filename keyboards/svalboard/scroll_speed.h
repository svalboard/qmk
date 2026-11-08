// SPDX-License-Identifier: GPL-2.0-or-later
// Scroll speed: one multiplier per hand for every path where hand motion
// becomes scroll (Scroll Mode, the Scroll Toggle keys, Drag Scroll, and a
// touchpad's two-finger scroll). Velocity Scroll has its own curve and is not
// multiplied.
//
// Motion becomes millimeters first, then wheel counts at a fixed base rate,
// then the hand's Scroll speed. The pointer's DPI never appears in the scroll
// math, so changing Pointer speed does not change scroll.
//
//   trackball   counts x 25.4 / DPI mm; 31.5 hi-res counts per mm at 1.0x,
//               the feel of 800 DPI before this existed
//   touchpad    pad pixels / 47.6 per mm across, 44.8 up and down; 170 pixels
//               per detent (120 counts) at 1.0x (pad_scroll.h)
//   TrackPoint  raw counts as the 800 DPI trackball feel (its DPI setting does
//               nothing)
//
// The OS divisors (a Mac counts each wheel count as a line) apply after the
// multiplier, so 2.0x means twice as fast on a Mac and on Windows.
//
// Stored as a percent, u16, 20 to 1000 (VIA values 61 left, 62 right).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_SCROLL_SPEED_MIN 20
#define SVAL_SCROLL_SPEED_MAX 1000
#define SVAL_SCROLL_SPEED_DEFAULT 100

// The DPI at which a trackball's scroll was 1 hi-res count per sensor count,
// and the divisor a Mac applies on top (keymaps/keymap_support.c, MAC_DIVISOR).
#define SVAL_BALL_SCROLL_BASE_DPI 800
#define SVAL_BALL_SCROLL_MAC_DIV 12

// The resolution at which the pad's pointer stops gaining (2048 x 1792 pixels
// over 43 x 40 mm, azo_num in azoteq.c): about 1210 DPI across, 1138 up and
// down. The biggest listed DPI at or below the across limit is the pad's
// maximum.
#define SVAL_PAD_CAP_DPI_X 1210
#define SVAL_PAD_CAP_DPI_Y 1138

uint16_t sval_scroll_speed_clamp(uint16_t pct);

// ---------------------------------------------------------------- storage
// Stored in the keyboard's custom config after the touchpad mode's record:
// left and right, little-endian, then a check byte. Bytes that don't check
// out (an erased or older board) are not a record, and the migration runs.
#define SVAL_SCROLL_SPEED_RECORD_BYTES 5
void sval_scroll_speed_record_write(uint16_t left, uint16_t right, uint8_t rec[SVAL_SCROLL_SPEED_RECORD_BYTES]);
bool sval_scroll_speed_record_read(const uint8_t rec[SVAL_SCROLL_SPEED_RECORD_BYTES], uint16_t *left, uint16_t *right);

// The test build's record (VIA value 60, the touchpad hand only): the percent
// and a check byte, three bytes in front of the record above. Read once, for
// the migration, then erased.
#define SVAL_SCROLL_SPEED_LEGACY_BYTES 3
bool sval_scroll_speed_legacy_read(const uint8_t rec[SVAL_SCROLL_SPEED_LEGACY_BYTES], uint16_t *pct);

// ---------------------------------------------------------------- migration
// The Scroll speed that keeps a hand's scroll as it was: its DPI over 800.
// A touchpad counts at most 1210 DPI (its pointer stops there, and scroll did
// too). A TrackPoint ignored its DPI setting, so it stays at 1.0x. Above 10x
// (8000 DPI) the speed stops at its maximum.
enum sval_scroll_kind { SVAL_SCROLL_KIND_BALL, SVAL_SCROLL_KIND_PAD, SVAL_SCROLL_KIND_TRACKPOINT };
uint16_t sval_scroll_speed_from_dpi(uint16_t dpi, uint8_t kind);
// The highest index of choices[n] at or below limit (0 if none).
uint8_t sval_dpi_index_at_most(const uint16_t *choices, uint8_t n, uint16_t limit);
// A saved DPI index lowered to the touchpad's maximum; indexes already at or
// below it stay.
uint8_t sval_dpi_index_normalize_pad(const uint16_t *choices, uint8_t n, uint8_t index);

// ---------------------------------------------------------------- trackball scaler
// The DPI that counts as the hand's motion resolution for scroll: the saved
// DPI for a trackball, capped per axis for a pad (its pointer counts stop
// gaining there), 800 for a TrackPoint.
uint16_t sval_scroll_motion_dpi(uint8_t kind, uint16_t dpi, bool vertical);

typedef struct {
    int32_t  rem_x, rem_y; // the part of a count not yet sent, in 1/den units
    uint16_t dpi_x, dpi_y; // the resolution and mode the remainders belong to
    bool     mac;
} sval_ball_scroll_t;

// Sensor counts in (x across, y as the wheel's vertical), the motion DPI per
// axis, the hand's Scroll speed (percent), Mac or not, wheel counts out.
void sval_ball_scroll(sval_ball_scroll_t *s, int32_t x, int32_t y, uint16_t dpi_x, uint16_t dpi_y, uint16_t pct, bool mac, int16_t *h, int16_t *v);

// One hand's migration, once its module is known: its Scroll speed from its
// saved DPI index (times the test build's touchpad speed, if there is one, for a pad)
// and the index normalized (a pad's lowered to its maximum). Speed first, from
// the DPI as it was.
void sval_scroll_migrate(uint8_t kind, uint8_t dpi_index, uint16_t legacy_pct, const uint16_t *choices, uint8_t n, uint16_t *speed, uint8_t *new_index);
