// Copyright 2026 Svalboard contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "action.h"
#include "action_layer.h"
#include "dynamic_keymap.h"
#include "timer.h"

#if defined(STRICT_LAYER_RELEASE) || defined(NO_ACTION_LAYER)
#    error "Sval context layers require QMK layers and the source-layer release cache"
#endif

#define CONTEXT_LAYER_LEASE_MS 5000
static uint8_t  app_layer = UINT8_MAX;
static uint16_t app_transaction;
static uint32_t app_renewed_at;

uint8_t sval_context_layer(void) {
    if (app_layer != UINT8_MAX && timer_elapsed32(app_renewed_at) >= CONTEXT_LAYER_LEASE_MS) {
        app_layer       = UINT8_MAX;
        app_transaction = 0;
    }
    return app_layer;
}

// QMK calls this first in layer_switch_get_layer() (quantum/sval_hooks.h,
// LAYER_SWITCH_GET_LAYER_OVERRIDE). The host owns a separate, temporary
// contribution. Never feed it into layer_state: MO/TG/TO and tri-layer hooks
// must operate only on manual state. UINT8_MAX leaves the key to QMK's normal
// lookup, which supplies the existing default/base fallback.
uint8_t layer_switch_get_layer_override(keypos_t key) {
    uint8_t layer = sval_context_layer();
    if (layer != UINT8_MAX) {
        layer_state_t manual = layer_state & ~(default_layer_state | (layer_state_t)1);
        for (int8_t i = MAX_LAYER - 1; i >= 0; i--) {
            if ((manual & ((layer_state_t)1 << i)) && action_for_key(i, key).code != ACTION_TRANSPARENT) return i;
        }
        if (action_for_key(layer, key).code != ACTION_TRANSPARENT) return layer;
    }
    return UINT8_MAX;
}

static void write_u32(uint8_t *data, uint32_t value) {
    for (uint8_t i = 0; i < 4; ++i)
        data[i] = value >> (8 * i);
}

bool sval_context_layer_command(uint8_t *data, uint8_t length) {
    if (length < 3) return false;
    uint8_t command = data[1];
    sval_context_layer();
    if (command == sval_cmd_context_layer_status) {
        if (length < 22) return false;
        uint32_t age       = timer_elapsed32(app_renewed_at);
        uint16_t remaining = app_layer != UINT8_MAX && age < CONTEXT_LAYER_LEASE_MS ? CONTEXT_LAYER_LEASE_MS - age : 0;
        data[2]            = 0;
        data[3]            = 1;
        data[4]            = DYNAMIC_KEYMAP_LAYER_COUNT;
        data[5]            = app_layer;
        data[6]            = app_transaction;
        data[7]            = app_transaction >> 8;
        data[8]            = remaining;
        data[9]            = remaining >> 8;
        write_u32(&data[10], layer_state);
        write_u32(&data[14], default_layer_state);
        uint32_t effective = layer_state | default_layer_state;
        if (app_layer != UINT8_MAX) effective |= (uint32_t)1 << app_layer;
        write_u32(&data[18], effective);
        return true;
    }
    if (command == sval_cmd_context_layer_clear) {
        app_layer       = UINT8_MAX;
        app_transaction = 0;
        data[2]         = 0;
        return true;
    }
    if (length < 4) return false;
    uint16_t transaction = data[2] | ((uint16_t)data[3] << 8);
    data[2]              = 1;
    if (!transaction) return true;
    if (command == sval_cmd_context_layer_set) {
        if (length < 5) return false;
        if (data[4] >= DYNAMIC_KEYMAP_LAYER_COUNT || data[4] >= MAX_LAYER) return true;
        app_layer       = data[4];
        app_transaction = transaction;
    } else if (command != sval_cmd_context_layer_renew || app_layer == UINT8_MAX || transaction != app_transaction) {
        return true;
    }
    app_renewed_at = timer_read32();
    data[2]        = 0;
    return true;
}
