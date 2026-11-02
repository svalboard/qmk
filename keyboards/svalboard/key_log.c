// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Key events on the debug console, only while debug is on (key_log.h).

#include "quantum.h"
#include "key_log.h"

void sval_key_log(uint16_t keycode, const keyrecord_t *record) {
    if (!debug_enable) return;
    uprintf("KL: kc: 0x%04X, col: %2u, row: %2u, pressed: %u, time: %5u, int: %u, count: %u\n", keycode, record->event.key.col, record->event.key.row, record->event.pressed, record->event.time, record->tap.interrupted, record->tap.count);
}
