// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Updater LED states (D24), shown on this half's WS2812 at SVAL_UPDATE_LED_VAL.
// While any of them is shown, rgblight's own output is held off (D20, see
// update_led.c); UPDATE_LED_NONE hands the LEDs back.

// One meaning per color, and red only for a failure: nothing a person should
// simply wait through is ever red.
typedef enum {
    UPDATE_LED_NONE = 0, // rgblight as usual: nothing for you to do
    UPDATE_LED_CONFIRM,  // blue blink: your turn, make the chord
    UPDATE_LED_CHORD,    // bright white double flash: got it, chord recognized
    UPDATE_LED_PROGRESS, // rainbow: updating, keep plugged in (both halves, until they match)
    UPDATE_LED_WRITING,  // solid magenta: this half's commit, which holds a fixed color
    UPDATE_LED_DONE,     // green double flash: done, the halves match
    UPDATE_LED_ERROR,    // red, 1 Hz, 50/50: it failed, open Keybard (stops after 30 s)
} update_led_mode_t;

// Called on every updater_task() pass with the mode for the current state.
void update_led_show(update_led_mode_t mode);

// Commit step 0: shows the writing color, latches it with two WS2812 flushes
// and waits until the last transfer has finished (its DMA reads RAM, so it is
// let run rather than stopped), so no DMA is running when the commit starts.
// The LEDs then keep that color until the reset.
void update_led_commit_latch(void);
