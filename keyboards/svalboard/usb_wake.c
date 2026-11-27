// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_wake.h"
#include "matrix.h"

// keyboard_task() has already processed these presses, so the matrix is read
// as it stands. QMK's suspend_wakeup_condition() would mark the keys as wakeup
// keys, whose releases keyboard.c then drops (it expects their presses to have
// been dropped as well, as its own suspend loop does): MO(n), LT() or a
// mod-tap's modifier stayed on after waking the host.
bool sval_usb_wake_key_down(void) {
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        if (matrix_get_row(row)) return true;
    }
    return false;
}
