// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "automouse_layers.h"

void sval_automouse_layers_record_write(uint8_t mode, uint16_t mask, uint8_t rec[SVAL_AUTOMOUSE_LAYERS_RECORD_BYTES]) {
    rec[0] = mode == SVAL_AUTOMOUSE_LAYERS_SELECTED ? SVAL_AUTOMOUSE_LAYERS_SELECTED : SVAL_AUTOMOUSE_LAYERS_EVERYWHERE;
    rec[1] = mask & 0xFF;
    rec[2] = mask >> 8;
    rec[3] = (uint8_t)(0xA5 ^ rec[0] ^ rec[1] ^ rec[2]);
}

bool sval_automouse_layers_record_read(const uint8_t rec[SVAL_AUTOMOUSE_LAYERS_RECORD_BYTES], uint8_t *mode, uint16_t *mask) {
    if (rec[0] > SVAL_AUTOMOUSE_LAYERS_SELECTED || (uint8_t)(0xA5 ^ rec[0] ^ rec[1] ^ rec[2]) != rec[3]) {
        *mode = SVAL_AUTOMOUSE_LAYERS_EVERYWHERE;
        *mask = 0;
        return false;
    }
    *mode = rec[0];
    *mask = (uint16_t)(rec[1] | (rec[2] << 8));
    return true;
}

uint8_t sval_automouse_current_layer(uint32_t layer_state, uint32_t default_layer_state, uint8_t mouse_layer) {
    uint32_t state = layer_state | default_layer_state;
    if (mouse_layer < 32) state &= ~((uint32_t)1 << mouse_layer);
    for (int8_t layer = 31; layer > 0; layer--) {
        if (state & ((uint32_t)1 << layer)) return (uint8_t)layer;
    }
    return 0;
}

bool sval_automouse_layer_blocked(uint8_t mode, uint16_t mask, uint32_t layer_state, uint32_t default_layer_state, uint8_t mouse_layer) {
    if (mode != SVAL_AUTOMOUSE_LAYERS_SELECTED || mask == 0) return false;
    uint8_t layer = sval_automouse_current_layer(layer_state, default_layer_state, mouse_layer);
    return layer < 16 && (mask >> layer) & 1;
}
