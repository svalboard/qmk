// SPDX-License-Identifier: GPL-2.0-or-later
// A grabbed touchpad finger as relative mouse motion (keymaps/keymap_support.c).
//
// While the firmware holds a pad (azoteq/ptp.c, sval_ptp_set_grab) it follows
// one finger's absolute position. This turns that position into the counts a
// trackball hand's report carries, at the hand's DPI, exactly as the pad's own
// mouse output scales them (azoteq/azoteq.c): the pad is 2048 x 1792 counts
// over 43 x 40 mm, and a DPI above the pad's full resolution adds nothing.
// Remainders carry, so slow motion is never lost. A new finger (a new touch
// serial) starts from where it was a sample before it was taken, or where it
// landed: never a jump, and no lost sample.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_PAD_FOLLOW_FULL_X 2048u
#define SVAL_PAD_FOLLOW_FULL_Y 1792u
#define SVAL_PAD_FOLLOW_MM_X 43u
#define SVAL_PAD_FOLLOW_MM_Y 40u

typedef struct {
    uint16_t touch; // the finger followed (0: none)
    uint16_t x, y;  // its last position, pad counts
    int32_t  rem_x, rem_y;
} sval_pad_follow_t;

// One step: the finger now (touch 0: none), where a new finger's motion
// starts (x0, y0: where it was a sample before it was taken, or where it
// landed), and the hand's DPI. Writes the motion since the last step, in
// counts at that DPI.
void sval_pad_follow(sval_pad_follow_t *f, uint16_t touch, uint16_t x, uint16_t y, uint16_t x0, uint16_t y0, uint16_t dpi, int16_t *dx, int16_t *dy);
