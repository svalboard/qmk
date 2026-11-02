// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Key events on the debug console. Any program on the host can read the
// console, so nothing is printed unless debug is on (debug_enable): off at
// every power-up, on only when a host asks (Sval 0x3E, debug_console.h) until
// the next power-up.
#pragma once

#include <stdint.h>
#include "action.h"

// The KL: line for one key event, while debug is on.
void sval_key_log(uint16_t keycode, const keyrecord_t *record);
