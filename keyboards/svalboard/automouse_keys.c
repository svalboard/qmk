// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "automouse_keys.h"
#include "keycodes.h"

bool sval_automouse_layer_key(uint16_t keycode, bool tapped, uint8_t mouse_layer) {
    uint8_t target;
    if (IS_QK_LAYER_TAP(keycode)) {
        if (tapped) return false;
        target = (keycode >> 8) & 0xF;
    } else if (IS_QK_LAYER_MOD(keycode)) {
        target = (keycode >> 5) & 0xF;
    } else if (IS_QK_TO(keycode) || IS_QK_MOMENTARY(keycode) || IS_QK_DEF_LAYER(keycode) || IS_QK_TOGGLE_LAYER(keycode) || IS_QK_ONE_SHOT_LAYER(keycode) || IS_QK_LAYER_TAP_TOGGLE(keycode) || IS_QK_PERSISTENT_DEF_LAYER(keycode)) {
        target = keycode & 0x1F;
    } else {
        return false;
    }
    return target != mouse_layer;
}

sval_am_key_t sval_automouse_key(uint16_t keycode, uint16_t layer_keycode, bool pressed, bool passthrough, bool layer_key, bool ends_swallowed) {
    if (passthrough) return SVAL_AM_KEY_NONE;
    if (layer_key) return pressed ? SVAL_AM_KEY_END_AFTER : SVAL_AM_KEY_NONE;
    if (ends_swallowed) return SVAL_AM_KEY_END_SWALLOW;
    // Transparent on the mouse layer: a key of the layers below, so it ends the mouse layer.
    if (layer_keycode != keycode) return SVAL_AM_KEY_END;
    return SVAL_AM_KEY_MOUSE;
}
