// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The touchpad chip's own gestures on the mouse path (pad_gestures.c): tap to
// click, two-finger tap for a right click, press and hold to drag, and the
// hold time. A saved setting of one byte; 0 is the build's own defaults
// (every Svalboard build: all three on, hold 300 ms), so a board that never
// changed it runs the build's defines and nothing more is written. Docs: docs/protocol.md "Touchpad taps".
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SVAL_PAD_TAP_OFF 0x01        // single tap does not click
#define SVAL_PAD_TWO_FINGER_OFF 0x02 // two-finger tap does not right click
#define SVAL_PAD_HOLD_OFF 0x04       // press and hold does not press the left button
#define SVAL_PAD_HOLD_SHIFT 3        // bits 3-6: index into sval_pad_hold_ms
#define SVAL_PAD_HOLD_MASK 0x78
#define SVAL_PAD_KNOWN 0x7F // bit 7 is never set in a setting
#define SVAL_PAD_HOLD_CHOICES 16

// Hold times the setting can hold, ms. Index 0 is the build default (300,
// AZOTEQ_IQS5XX_HOLD_TIME), so a stored byte only ever records a change from
// it; the rest rise.
extern const uint16_t sval_pad_hold_ms[SVAL_PAD_HOLD_CHOICES];

bool     sval_pad_tap_on(uint8_t s);
bool     sval_pad_two_finger_on(uint8_t s);
bool     sval_pad_hold_on(uint8_t s);
uint16_t sval_pad_hold_time_ms(uint8_t s);
// A switch turned on or off; the other parts of the setting are kept.
uint8_t sval_pad_with(uint8_t s, uint8_t off_bit, bool on);
// The hold time nearest to ms (a tie takes the shorter one).
uint8_t sval_pad_with_hold_ms(uint8_t s, uint16_t ms);

// Stored as the byte and its complement, after the keyboard settings' magic
// (svalboard.c): two bytes that don't check out (never written, or left by
// other firmware) read as 0, the defaults.
#define SVAL_PAD_RECORD_BYTES 2
void    sval_pad_record_write(uint8_t s, uint8_t rec[SVAL_PAD_RECORD_BYTES]);
uint8_t sval_pad_record_read(const uint8_t rec[SVAL_PAD_RECORD_BYTES]);

// The chip's gesture registers from 0x06B7, the first 8 bytes: single-finger
// enables, multi-finger enables, tap time, tap distance, hold time (16-bit
// big-endian). Sets the three enables and the hold time; every other bit
// (swipes, scroll, zoom, the times and distances) is left as read.
#define SVAL_PAD_REG_GESTURES 0x06B7
#define SVAL_PAD_REG_BYTES 8
void sval_pad_regs_apply(uint8_t s, uint8_t regs[SVAL_PAD_REG_BYTES]);

// Split sync. The half with USB owns the setting and sends this byte in its
// presence ping; the other half uses it. present: the ping was long enough
// to hold the byte (an older half with USB sends a shorter ping).
#define SVAL_PAD_SYNC_VALID 0x80
uint8_t sval_pad_sync_byte(uint8_t s);
uint8_t sval_pad_sync_setting(bool present, uint8_t sync_byte, uint8_t current);

// ---------------------------------------------------------------- tuning
//
// The touchpad tuning (VIA values 55-57, sval_flag3_pad_tuning): the dead
// zone a tap or press and hold must stay inside (the chip's tap distance),
// the tap time, and how far two fingers move before they scroll (the chip's
// scroll initial distance). The distances are physical, in 0.01 mm: the chip
// counts them in pixels of its output resolution, which differs between
// builds (the native touchpad keeps the full 2048 across the pad, the builds
// without it scale it to the pointer DPI), so the firmware converts them from
// the resolution the chip has. A 16-bit setting: bits 0-3 the dead zone,
// bits 4-7 the tap time, bits 8-11 the scroll distance, each an index into
// its table; bits 12-15 zero. 0 is the defaults: a 1.2 mm dead zone, a
// 150 ms tap time and the build's own scroll distance (50 pixels).
#define SVAL_PAD_TUNE_CHOICES 16
#define SVAL_PAD_TUNE_KNOWN 0x0FFF

// The TPS43 pad, 0.01 mm (the driver's AZOTEQ_IQS5XX_WIDTH_MM / HEIGHT_MM).
#define SVAL_PAD_WIDTH_MM100 4300
#define SVAL_PAD_HEIGHT_MM100 4000

// Index 0 is each default; the rest rise. A scroll distance of 0 is the
// build's own (the driver's AZOTEQ_IQS5XX_SCROLL_INITIAL_DISTANCE, in pixels).
extern const uint16_t sval_pad_dead_zone_mm100[SVAL_PAD_TUNE_CHOICES];
extern const uint16_t sval_pad_tap_ms[SVAL_PAD_TUNE_CHOICES];
extern const uint16_t sval_pad_scroll_mm100[SVAL_PAD_TUNE_CHOICES];

uint16_t sval_pad_dead_zone(uint16_t t);    // 0.01 mm
uint16_t sval_pad_tap_time_ms(uint16_t t);  // ms
uint16_t sval_pad_scroll_start(uint16_t t); // 0.01 mm, 0 = the build's own
// The nearest choice (a tie takes the smaller); the other parts are kept. A
// scroll distance of 0 picks the build's own.
uint16_t sval_pad_tune_with_dead_zone(uint16_t t, uint16_t mm100);
uint16_t sval_pad_tune_with_tap_ms(uint16_t t, uint16_t ms);
uint16_t sval_pad_tune_with_scroll(uint16_t t, uint16_t mm100);

// A distance in 0.01 mm as chip pixels, at res pixels across span_mm100
// (rounded; at least 1), and back.
uint16_t sval_pad_mm100_to_px(uint16_t mm100, uint16_t res, uint16_t span_mm100);
uint16_t sval_pad_px_to_mm100(uint16_t px, uint16_t res, uint16_t span_mm100);

// Stored after the tap setting's record: the setting, little-endian, then
// its complement. Four bytes that don't check out read as 0, the defaults.
#define SVAL_PAD_TUNE_RECORD_BYTES 4
void     sval_pad_tune_record_write(uint16_t t, uint8_t rec[SVAL_PAD_TUNE_RECORD_BYTES]);
uint16_t sval_pad_tune_record_read(const uint8_t rec[SVAL_PAD_TUNE_RECORD_BYTES]);

// The chip's gesture registers from 0x06B7 through the scroll distance at
// 0x06C8: 19 bytes, the first 8 as above. Sets the tap time, the tap
// distance and the scroll initial distance (16-bit big-endian), in pixels
// at xres across the pad; scroll_build_px is the build's own scroll distance.
// Every other byte is left as read.
#define SVAL_PAD_TUNE_REG_BYTES 19
void sval_pad_tune_regs_apply(uint16_t t, uint16_t xres, uint16_t scroll_build_px, uint8_t regs[SVAL_PAD_TUNE_REG_BYTES]);

// Split sync, as the tap setting's: bit 15 marks a setting.
#define SVAL_PAD_TUNE_SYNC_VALID 0x8000
uint16_t sval_pad_tune_sync_word(uint16_t t);
uint16_t sval_pad_tune_sync_setting(bool present, uint16_t sync_word, uint16_t current);

// ---------------------------------------------------------------- palm rejection
//
// The touchpad palm rejection (VIA value 58, sval_flag4_pad_palm; native
// touchpad builds): a contact whose area, in the chip's channels (about
// 5.4 x 5.7 mm each on the TPS43), is above this is a palm. 0 is off, the
// default (what every build did before it). One byte, 0-56 (the TPS43 has
// 8 x 7 channels).
#define SVAL_PAD_PALM_MAX 56
uint8_t sval_pad_palm_clamp(uint16_t area);

// Stored after the tuning's record: the byte and its complement. Two bytes
// that don't check out, or a value above SVAL_PAD_PALM_MAX, read as 0 (off).
#define SVAL_PAD_PALM_RECORD_BYTES 2
void    sval_pad_palm_record_write(uint8_t palm, uint8_t rec[SVAL_PAD_PALM_RECORD_BYTES]);
uint8_t sval_pad_palm_record_read(const uint8_t rec[SVAL_PAD_PALM_RECORD_BYTES]);

// Split sync, as the tap setting's: bit 7 marks a setting.
#define SVAL_PAD_PALM_SYNC_VALID 0x80
uint8_t sval_pad_palm_sync_byte(uint8_t palm);
uint8_t sval_pad_palm_sync_setting(bool present, uint8_t sync_byte, uint8_t current);

// A contact of this area is a palm under this setting.
static inline bool sval_pad_is_palm(uint8_t palm, uint8_t area) {
    return palm && area > palm;
}
