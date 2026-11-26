// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What a key does to the auto mouse layer while it is on (keymap_support.c,
// process_record_kb).
//
// The auto mouse layer (15) is the highest layer, so a layer a key turns on
// while it is on sits under it, and every key still reads the mouse layer
// first. A layer key the user put on the mouse layer itself used to count as
// a mouse key: the mouse layer stayed on (and stopped timing out while the
// key was held), so MO(1) seemed to do nothing, and TO(1) left the mouse
// layer's held-key count stuck, so it never timed out again.
//
// Now a key on the mouse layer that changes to another layer ends the mouse
// layer when it is pressed, once its own action has run (post_process_record_kb:
// QMK looks the action up after process_record_kb, so ending the mouse layer
// any earlier would run the key below it instead), and so does what it does
// from any other layer:
// MO(1) holds layer 1 until it is released, TO(1) moves to layer 1. Its
// release leaves the mouse layer alone. A key whose target is the mouse
// layer itself stays a mouse key.
//
// Layer keys: MO, LM, TT, OSL, TO, TG, DF and PDF, and LT once it is held
// (an LT tapped is its tap key, so it stays a mouse key). Svalboard's own
// layer holds (SV_TOGGLE_23_67, SV_TOGGLE_45_67) are passed in as layer keys
// by the caller.
//
// Pure functions: only quantum/keycodes.h, so the host tests build them as
// they are.

#include <stdbool.h>
#include <stdint.h>

// True when pressing keycode on the mouse layer must end the mouse layer
// first. tapped is record->tap.count > 0.
bool sval_automouse_layer_key(uint16_t keycode, bool tapped, uint8_t mouse_layer);

typedef enum {
    SVAL_AM_KEY_NONE,         // leaves the mouse layer as it is (passthrough keys, a layer key's release)
    SVAL_AM_KEY_MOUSE,        // a key of the mouse layer's own: counts as held, refreshes the timeout
    SVAL_AM_KEY_END,          // ends the mouse layer, then runs (as the key of the layer below it shows through to)
    SVAL_AM_KEY_END_AFTER,    // runs as the mouse layer's own key, then ends the mouse layer (a layer key)
    SVAL_AM_KEY_END_SWALLOW,  // ends the mouse layer and does nothing else (Blank, and the keys in BAD_KEYCODE_CONDITONAL)
} sval_am_key_t;

// keycode is the key as resolved through the active layers; layer_keycode is
// what the mouse layer itself has at that position (KC_TRNS shows through to
// the layers below, so the two differ). passthrough and ends_swallowed are
// the caller's lists (MOUSE_PASSTHROUGH_KEYCODE, BAD_KEYCODE_CONDITONAL);
// layer_key is sval_automouse_layer_key() or one of the caller's own.
sval_am_key_t sval_automouse_key(uint16_t keycode, uint16_t layer_keycode, bool pressed, bool passthrough, bool layer_key, bool ends_swallowed);
