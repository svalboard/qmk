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

// ---------------------------------------------------------------- tuning

const uint16_t sval_pad_dead_zone_mm100[SVAL_PAD_TUNE_CHOICES] = {120, 20, 30, 40, 50, 60, 70, 80, 90, 100, 140, 160, 200, 250, 300, 400};
const uint16_t sval_pad_tap_ms[SVAL_PAD_TUNE_CHOICES]          = {150, 50, 75, 100, 125, 175, 200, 225, 250, 300, 350, 400, 500, 600, 800, 1000};
const uint16_t sval_pad_scroll_mm100[SVAL_PAD_TUNE_CHOICES]    = {0, 30, 50, 75, 100, 125, 150, 175, 200, 250, 300, 400, 500, 600, 800, 1000};

#define TUNE_DEAD_SHIFT 0
#define TUNE_TAP_SHIFT 4
#define TUNE_SCROLL_SHIFT 8

static uint8_t tune_index(uint16_t t, uint8_t shift) {
    return (t >> shift) & 0x0F;
}

static uint16_t tune_set(uint16_t t, uint8_t shift, uint8_t i) {
    return (uint16_t)(((t & ~(0x0Fu << shift)) | ((uint16_t)i << shift)) & SVAL_PAD_TUNE_KNOWN);
}

// The nearest entry from index first on; a tie takes the smaller value.
static uint8_t nearest(const uint16_t *table, uint8_t first, uint16_t v) {
    uint8_t  best      = first;
    uint16_t best_diff = UINT16_MAX;
    for (uint8_t i = first; i < SVAL_PAD_TUNE_CHOICES; i++) {
        uint16_t d = v > table[i] ? v - table[i] : table[i] - v;
        if (d < best_diff || (d == best_diff && table[i] < table[best])) {
            best      = i;
            best_diff = d;
        }
    }
    return best;
}

uint16_t sval_pad_dead_zone(uint16_t t) {
    return sval_pad_dead_zone_mm100[tune_index(t, TUNE_DEAD_SHIFT)];
}

uint16_t sval_pad_tap_time_ms(uint16_t t) {
    return sval_pad_tap_ms[tune_index(t, TUNE_TAP_SHIFT)];
}

uint16_t sval_pad_scroll_start(uint16_t t) {
    return sval_pad_scroll_mm100[tune_index(t, TUNE_SCROLL_SHIFT)];
}

uint16_t sval_pad_tune_with_dead_zone(uint16_t t, uint16_t mm100) {
    return tune_set(t, TUNE_DEAD_SHIFT, nearest(sval_pad_dead_zone_mm100, 0, mm100));
}

uint16_t sval_pad_tune_with_tap_ms(uint16_t t, uint16_t ms) {
    return tune_set(t, TUNE_TAP_SHIFT, nearest(sval_pad_tap_ms, 0, ms));
}

uint16_t sval_pad_tune_with_scroll(uint16_t t, uint16_t mm100) {
    return tune_set(t, TUNE_SCROLL_SHIFT, mm100 == 0 ? 0 : nearest(sval_pad_scroll_mm100, 1, mm100));
}

uint16_t sval_pad_mm100_to_px(uint16_t mm100, uint16_t res, uint16_t span_mm100) {
    if (!span_mm100) return 1;
    uint32_t px = ((uint32_t)mm100 * res + span_mm100 / 2) / span_mm100;
    if (px < 1) px = 1;
    return px > UINT16_MAX ? UINT16_MAX : (uint16_t)px;
}

uint16_t sval_pad_px_to_mm100(uint16_t px, uint16_t res, uint16_t span_mm100) {
    if (!res) return 0;
    uint32_t mm = ((uint32_t)px * span_mm100 + res / 2) / res;
    return mm > UINT16_MAX ? UINT16_MAX : (uint16_t)mm;
}

void sval_pad_tune_record_write(uint16_t t, uint8_t rec[SVAL_PAD_TUNE_RECORD_BYTES]) {
    t &= SVAL_PAD_TUNE_KNOWN;
    rec[0] = t & 0xFF;
    rec[1] = t >> 8;
    rec[2] = (uint8_t)~rec[0];
    rec[3] = (uint8_t)~rec[1];
}

uint16_t sval_pad_tune_record_read(const uint8_t rec[SVAL_PAD_TUNE_RECORD_BYTES]) {
    if ((uint8_t)(rec[0] ^ rec[2]) != 0xFF || (uint8_t)(rec[1] ^ rec[3]) != 0xFF) return 0;
    uint16_t t = rec[0] | (uint16_t)rec[1] << 8;
    return (t & ~SVAL_PAD_TUNE_KNOWN) ? 0 : t;
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

void sval_pad_tune_regs_apply(uint16_t t, uint16_t xres, uint16_t scroll_build_px, uint8_t regs[SVAL_PAD_TUNE_REG_BYTES]) {
    put_be16(&regs[2], sval_pad_tap_time_ms(t));                                                 // 0x06B9 tap time
    put_be16(&regs[4], sval_pad_mm100_to_px(sval_pad_dead_zone(t), xres, SVAL_PAD_WIDTH_MM100)); // 0x06BB tap distance
    uint16_t scroll = sval_pad_scroll_start(t);
    put_be16(&regs[17], scroll ? sval_pad_mm100_to_px(scroll, xres, SVAL_PAD_WIDTH_MM100) : scroll_build_px); // 0x06C8
}

uint16_t sval_pad_tune_sync_word(uint16_t t) {
    return SVAL_PAD_TUNE_SYNC_VALID | (t & SVAL_PAD_TUNE_KNOWN);
}

uint16_t sval_pad_tune_sync_setting(bool present, uint16_t sync_word, uint16_t current) {
    if (!present || !(sync_word & SVAL_PAD_TUNE_SYNC_VALID)) return current;
    return sync_word & SVAL_PAD_TUNE_KNOWN;
}

void sval_pad_mode_record_write(uint8_t mode, uint8_t rec[SVAL_PAD_MODE_RECORD_BYTES]) {
    rec[0] = mode == SVAL_PAD_MODE_BUILTIN ? SVAL_PAD_MODE_BUILTIN : SVAL_PAD_MODE_AUTO;
    rec[1] = (uint8_t)~rec[0];
}

uint8_t sval_pad_mode_record_read(const uint8_t rec[SVAL_PAD_MODE_RECORD_BYTES]) {
    if ((uint8_t)(rec[0] ^ rec[1]) != 0xFF || rec[0] > SVAL_PAD_MODE_BUILTIN) return SVAL_PAD_MODE_AUTO;
    return rec[0];
}

uint8_t sval_pad_mode_sync_byte(uint8_t mode) {
    return SVAL_PAD_MODE_SYNC_VALID | (mode == SVAL_PAD_MODE_BUILTIN ? 1 : 0);
}

uint8_t sval_pad_mode_sync_setting(bool present, uint8_t sync_byte, uint8_t current) {
    if (!present || !(sync_byte & SVAL_PAD_MODE_SYNC_VALID)) return current;
    return (sync_byte & 0x7F) == SVAL_PAD_MODE_BUILTIN ? SVAL_PAD_MODE_BUILTIN : SVAL_PAD_MODE_AUTO;
}

uint8_t sval_pad_palm_clamp(uint16_t area) {
    return area > SVAL_PAD_PALM_MAX ? SVAL_PAD_PALM_MAX : (uint8_t)area;
}

void sval_pad_palm_record_write(uint8_t palm, uint8_t rec[SVAL_PAD_PALM_RECORD_BYTES]) {
    rec[0] = sval_pad_palm_clamp(palm);
    rec[1] = (uint8_t)~rec[0];
}

uint8_t sval_pad_palm_record_read(const uint8_t rec[SVAL_PAD_PALM_RECORD_BYTES]) {
    if ((uint8_t)(rec[0] ^ rec[1]) != 0xFF || rec[0] > SVAL_PAD_PALM_MAX) return 0;
    return rec[0];
}

uint8_t sval_pad_palm_sync_byte(uint8_t palm) {
    return SVAL_PAD_PALM_SYNC_VALID | sval_pad_palm_clamp(palm);
}

uint8_t sval_pad_palm_sync_setting(bool present, uint8_t sync_byte, uint8_t current) {
    if (!present || !(sync_byte & SVAL_PAD_PALM_SYNC_VALID)) return current;
    return sval_pad_palm_clamp(sync_byte & 0x7F);
}

uint8_t sval_palm_mouse_buttons(sval_palm_mouse_t *s, bool palm_before, bool palm_after, uint8_t before, uint8_t now) {
    // The latch clears in the all-up read, which is also the read that carries
    // a tap or a two-finger tap: a palm before or after this read both count.
    if (!palm_before && !palm_after) {
        s->on = false;
        return now;
    }
    if (!s->on) {
        s->on      = true;
        s->buttons = before;
    }
    // Only buttons the driver still holds: a drag ends when the chip ends it.
    // Sending a button the driver let go would hand it back as "outside" (a
    // mouse key's), held for good.
    s->buttons &= now;
    return s->buttons;
}

uint8_t sval_pad_palm_from_value(const uint8_t value[2]) {
    return sval_pad_palm_clamp((uint16_t)(value[0] | (value[1] << 8)));
}
