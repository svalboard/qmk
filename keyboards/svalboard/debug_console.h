// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// DEBUG_CONSOLE (Sval 0x3E, docs/protocol.md "Debug console"): a host turns
// QMK's debug flag (debug_enable) on or off and reads it. Key events print on
// the debug console only while it is on (key_log.h). Never saved: debug is
// off again at every power-up.
#pragma once

#include <stdint.h>

enum {
    SVAL_DEBUG_OFF  = 0,
    SVAL_DEBUG_ON   = 1,
    SVAL_DEBUG_READ = 2,
};

enum {
    SVAL_DEBUG_DONE       = 0,
    SVAL_DEBUG_UNKNOWN_OP = 1,
    SVAL_DEBUG_NO_CONSOLE = 2, // the build has no console (CONSOLE_ENABLE off)
    SVAL_DEBUG_TOO_SHORT  = 3,
};

// data: [op] in; [op] [status] [state] out (length >= 3).
void sval_debug_console_kb(uint8_t *data, uint8_t length);
