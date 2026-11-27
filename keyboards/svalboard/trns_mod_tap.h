// SPDX-License-Identifier: GPL-2.0-or-later
// A mod-tap whose tap is KC_TRNS, MT(mod, KC_TRNS) (keymaps/keymap_support.c):
// its hold is the modifier and its tap is the key under it. On the tap's press
// the layer the key resolves on is turned off, so QMK resolves the press from
// the layers below; the next release turns it back on.
#pragma once
#include "quantum.h"

// The layer to turn off for this event, when it is the tap's press of a
// transparent mod-tap and none is running; else -1.
int8_t sval_trns_mod_tap_layer(uint16_t keycode, const keyrecord_t *record);
// Turns that layer off until the next release.
void sval_trns_mod_tap_begin(uint8_t layer);
// A release while one runs turns the layer back on: true then, and the caller
// lets QMK go on with the release.
bool sval_trns_mod_tap_end(const keyrecord_t *record);
