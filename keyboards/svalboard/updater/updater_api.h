// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the rest of the keyboard calls in the updater, in builds with and
// without it. With SVAL_UPDATER each call forwards to the updater; without it
// each one is an inline false or does nothing, so callers need no #ifdef.
// Fields that change a data layout (the presence ping and reply,
// svalboard.h) and the split transaction table (config.h) stay compile-time.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef SVAL_UPDATER
#    include "keyboard.h" // is_keyboard_master
#    include "updater/updater.h"
#    include "updater/update_gesture.h"
#    include "updater/update_split.h"
#    include "split_pause.h"

// An update session, relay or pair relay is running: it owns the split link,
// the LEDs and the flash.
static inline bool updater_busy(void) {
    return updater_active();
}

// The split link is paused for an update (D15, split_pause.h).
static inline bool updater_split_paused(void) {
    return sval_split_paused();
}

// keyboard_post_init_kb, before kb_sync_listener can answer a presence ping.
static inline void updater_kb_init(void) {
    update_split_init();
}

// housekeeping_task_kb. On the half without USB, the session the other half
// runs over KEYBOARD_UPDATE does its slow work here first (M2).
static inline void updater_kb_task(void) {
    if (!is_keyboard_master()) update_split_slave_task();
    updater_task();
}

// The presence ping to the other half went unanswered.
static inline void updater_kb_presence_lost(void) {
    update_split_presence_result(false, NULL, 0);
}

// A VIA custom-value command: true if it was the updater's (UPDATE_CHANNEL)
// and has been handled.
static inline bool updater_kb_via_command(uint8_t *data, uint8_t length) {
    if (data[1] == UPDATE_CHANNEL) {
        updater_via_command(data, length);
        return true;
    }
    return false;
}

// The matrix scan: the chord's scan timing, and the chord read (its keys
// removed) before debounce sees the frame.
static inline void updater_matrix_timing(uint16_t *prewait_us, uint16_t *postwait_us) {
    update_gesture_timing(prewait_us, postwait_us);
}
static inline void updater_matrix_scan(matrix_row_t *local_rows) {
    update_gesture_scan(local_rows);
}

#else

static inline bool updater_busy(void) {
    return false;
}
static inline bool updater_split_paused(void) {
    return false;
}
static inline void updater_kb_init(void) {}
static inline void updater_kb_task(void) {}
static inline void updater_kb_presence_lost(void) {}
static inline bool updater_kb_via_command(uint8_t *data, uint8_t length) {
    (void)data;
    (void)length;
    return false;
}
static inline void updater_matrix_timing(uint16_t *prewait_us, uint16_t *postwait_us) {
    (void)prewait_us;
    (void)postwait_us;
}
// void *: matrix.h is not needed here, so host tests can include this file.
static inline void updater_matrix_scan(void *local_rows) {
    (void)local_rows;
}

#endif
