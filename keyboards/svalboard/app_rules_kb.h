// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// App rules on the board (app_rules_kb.c; the rules themselves: app_rules.h).

#include <stdint.h>
#include "keyboard.h"

#define SVAL_APP_RULES_CHUNK 48 // bytes per READ or WRITE

enum sval_app_foreground_op {
    SVAL_APP_FG_STATUS = 0,
    SVAL_APP_FG_SET    = 1,
    SVAL_APP_FG_CLEAR  = 2,
};

enum sval_app_rules_op {
    SVAL_APP_RULES_INFO   = 0,
    SVAL_APP_RULES_READ   = 1,
    SVAL_APP_RULES_WRITE  = 2,
    SVAL_APP_RULES_BEGIN  = 3,
    SVAL_APP_RULES_COMMIT = 4,
};

enum sval_app_status {
    SVAL_APP_STATUS_OK        = 0,
    SVAL_APP_STATUS_BAD       = 1, // unknown op, or offset and length out of range
    SVAL_APP_STATUS_NOT_USB   = 2, // not the half with USB
    SVAL_APP_STATUS_SHORT     = 3, // report too short
    SVAL_APP_STATUS_INVALID   = 4, // COMMIT: the edited record has a bad header
    SVAL_APP_STATUS_NOT_SAVED = 5, // COMMIT: in use, but could not be saved to flash
};

// Read the rules from the settings area (at startup, after the area is valid).
void sval_app_rules_load(void);
// From housekeeping: expiry of the foreground app, on the half with USB.
void sval_app_rules_task(void);
// The layer for a key while a rule changes key lookup, else UINT8_MAX.
uint8_t sval_app_rules_lookup(keypos_t key);
// The layers on as the rules leave them: (state | on) & ~off, plus the front layer.
uint32_t sval_app_rules_layer_view(uint32_t state);
// The module hooks (sval.h, sval_context_layer.c) svalboard.c fills in.
uint8_t sval_layer_lookup_kb(keypos_t key);
void     sval_app_foreground_command(uint8_t *data, uint8_t length);
void     sval_app_rules_command(uint8_t *data, uint8_t length);
