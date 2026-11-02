// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// DEBUG_CONSOLE (Sval 0x3E): set and read debug_enable (debug_console.h).

#include "quantum.h"
#include "debug_console.h"

void sval_debug_console_kb(uint8_t *data, uint8_t length) {
    if (length < 3) {
        if (length > 1) data[1] = SVAL_DEBUG_TOO_SHORT;
        return;
    }
    const uint8_t op = data[0];
    uint8_t       status = SVAL_DEBUG_DONE;
#ifdef CONSOLE_ENABLE
    if (op == SVAL_DEBUG_OFF || op == SVAL_DEBUG_ON) {
        // RAM only, never eeconfig_update_debug: off again at the next
        // power-up (keyboard_post_init_kb also clears it).
        debug_enable = op == SVAL_DEBUG_ON;
    } else if (op != SVAL_DEBUG_READ) {
        status = SVAL_DEBUG_UNKNOWN_OP;
    }
#else
    status = op <= SVAL_DEBUG_READ ? SVAL_DEBUG_NO_CONSOLE : SVAL_DEBUG_UNKNOWN_OP;
#endif
    data[1] = status;
    data[2] = debug_enable ? 1 : 0;
}
