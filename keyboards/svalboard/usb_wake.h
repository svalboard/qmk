// SPDX-License-Identifier: GPL-2.0-or-later
// Waking a suspended host (svalboard.c, sval_usb_wake_handler). With
// NO_USB_STARTUP_CHECK, QMK's own suspend loop is gone and the keyboard keeps
// scanning while the host sleeps; the handler runs from housekeeping, after
// keyboard_task() has scanned and processed the matrix.
#pragma once
#include <stdbool.h>

// A key is down to wake the host with.
bool sval_usb_wake_key_down(void);
