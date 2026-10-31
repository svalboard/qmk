// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The touchpad tap setting (pad_gestures.h). Pure logic, host tested in
// tests/sval_pad_gestures; svalboard.c stores and syncs it, azoteq/azoteq.c
// writes it to the chip.
#include "pad_gestures.h"

const uint16_t sval_pad_hold_ms[SVAL_PAD_HOLD_CHOICES] = {300, 20, 50, 100, 150, 200, 250, 350, 400, 500, 600, 700, 800, 1000, 1200, 1500};

bool sval_pad_tap_on(uint8_t s) {
    return !(s & SVAL_PAD_TAP_OFF);
}

bool sval_pad_two_finger_on(uint8_t s) {
    return !(s & SVAL_PAD_TWO_FINGER_OFF);
}

bool sval_pad_hold_on(uint8_t s) {
    return !(s & SVAL_PAD_HOLD_OFF);
}

uint16_t sval_pad_hold_time_ms(uint8_t s) {
    return sval_pad_hold_ms[(s & SVAL_PAD_HOLD_MASK) >> SVAL_PAD_HOLD_SHIFT];
}

uint8_t sval_pad_with(uint8_t s, uint8_t off_bit, bool on) {
    off_bit &= SVAL_PAD_TAP_OFF | SVAL_PAD_TWO_FINGER_OFF | SVAL_PAD_HOLD_OFF;
    return (uint8_t)((on ? (s & ~off_bit) : (s | off_bit)) & SVAL_PAD_KNOWN);
}

uint8_t sval_pad_with_hold_ms(uint8_t s, uint16_t ms) {
    // Nearest; a tie takes the shorter time (the table is not in order: the
    // default comes first).
    uint8_t  best      = 0;
    uint16_t best_diff = UINT16_MAX;
    for (uint8_t i = 0; i < SVAL_PAD_HOLD_CHOICES; i++) {
        uint16_t d = ms > sval_pad_hold_ms[i] ? ms - sval_pad_hold_ms[i] : sval_pad_hold_ms[i] - ms;
        if (d < best_diff || (d == best_diff && sval_pad_hold_ms[i] < sval_pad_hold_ms[best])) {
            best      = i;
            best_diff = d;
        }
    }
    return (uint8_t)(((s & ~SVAL_PAD_HOLD_MASK) | (best << SVAL_PAD_HOLD_SHIFT)) & SVAL_PAD_KNOWN);
}

void sval_pad_record_write(uint8_t s, uint8_t rec[SVAL_PAD_RECORD_BYTES]) {
    rec[0] = s & SVAL_PAD_KNOWN;
    rec[1] = (uint8_t)~rec[0];
}

uint8_t sval_pad_record_read(const uint8_t rec[SVAL_PAD_RECORD_BYTES]) {
    if ((uint8_t)(rec[0] ^ rec[1]) != 0xFF || (rec[0] & ~SVAL_PAD_KNOWN)) return 0;
    return rec[0];
}

void sval_pad_regs_apply(uint8_t s, uint8_t regs[SVAL_PAD_REG_BYTES]) {
    // Single-finger enables: bit 0 single tap, bit 1 press and hold.
    regs[0] = (uint8_t)((regs[0] & ~0x03) | (sval_pad_tap_on(s) ? 0x01 : 0) | (sval_pad_hold_on(s) ? 0x02 : 0));
    // Multi-finger enables: bit 0 two-finger tap.
    regs[1]       = (uint8_t)((regs[1] & ~0x01) | (sval_pad_two_finger_on(s) ? 0x01 : 0));
    uint16_t hold = sval_pad_hold_time_ms(s);
    regs[6]       = (uint8_t)(hold >> 8);
    regs[7]       = (uint8_t)(hold & 0xFF);
}

uint8_t sval_pad_sync_byte(uint8_t s) {
    return SVAL_PAD_SYNC_VALID | (s & SVAL_PAD_KNOWN);
}

uint8_t sval_pad_sync_setting(bool present, uint8_t sync_byte, uint8_t current) {
    if (!present || !(sync_byte & SVAL_PAD_SYNC_VALID)) return current;
    return sync_byte & SVAL_PAD_KNOWN;
}
