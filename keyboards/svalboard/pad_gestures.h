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

#define SVAL_PAD_TAP_OFF        0x01 // single tap does not click
#define SVAL_PAD_TWO_FINGER_OFF 0x02 // two-finger tap does not right click
#define SVAL_PAD_HOLD_OFF       0x04 // press and hold does not press the left button
#define SVAL_PAD_HOLD_SHIFT     3    // bits 3-6: index into sval_pad_hold_ms
#define SVAL_PAD_HOLD_MASK      0x78
#define SVAL_PAD_KNOWN          0x7F // bit 7 is never set in a setting
#define SVAL_PAD_HOLD_CHOICES   16

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
#define SVAL_PAD_REG_GESTURES   0x06B7
#define SVAL_PAD_REG_BYTES      8
void sval_pad_regs_apply(uint8_t s, uint8_t regs[SVAL_PAD_REG_BYTES]);

// Split sync. The half with USB owns the setting and sends this byte in its
// presence ping; the other half uses it. present: the ping was long enough
// to hold the byte (an older half with USB sends a shorter ping).
#define SVAL_PAD_SYNC_VALID 0x80
uint8_t sval_pad_sync_byte(uint8_t s);
uint8_t sval_pad_sync_setting(bool present, uint8_t sync_byte, uint8_t current);
