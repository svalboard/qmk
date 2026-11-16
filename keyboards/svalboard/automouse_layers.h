// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Auto Mouse layers: the layers where pointer motion does not switch to the
// auto mouse layer (VIA values 63 and 64, sval_flag4_automouse_layers).
//
// Two settings: the mode (0 Everywhere, the default and the behavior before
// this setting; 1 Only on Selected Layers) and a 16-bit mask of the layers
// where Auto Mouse is off. The mask counts only in mode 1, so turning the
// mode off and on again keeps the layers picked.
//
// The layer that counts is the highest active layer other than the auto mouse
// layer itself, with the default layer included: the layer the user is on
// now, whether they reached it by a held key, a toggle or the default layer.
// While it is a disabled layer, motion does not turn the mouse layer on, and
// a mouse layer already on goes off (once no mouse button is held).
//
// Pure functions: no QMK headers, so the host tests build them as they are.

#include <stdbool.h>
#include <stdint.h>

#define SVAL_AUTOMOUSE_LAYERS_EVERYWHERE 0
#define SVAL_AUTOMOUSE_LAYERS_SELECTED 1

// The record (settings area id 9, SVAL_SET_AUTOMOUSE_LAYERS):
// [mode][mask lo][mask hi][check]. check = 0xA5 ^ mode ^ lo ^ hi. Bytes never
// written (all 0x00 or all 0xFF) fail the check and read
// as Everywhere with no layers, so existing boards keep today's behavior.
#define SVAL_AUTOMOUSE_LAYERS_RECORD_BYTES 4

void sval_automouse_layers_record_write(uint8_t mode, uint16_t mask, uint8_t rec[SVAL_AUTOMOUSE_LAYERS_RECORD_BYTES]);
// Returns false (and sets Everywhere, mask 0) for a record that fails the check.
bool sval_automouse_layers_record_read(const uint8_t rec[SVAL_AUTOMOUSE_LAYERS_RECORD_BYTES], uint8_t *mode, uint16_t *mask);

// The highest active layer, the auto mouse layer left out; the default layer
// state counts too, so the base layer is found when nothing else is on.
uint8_t sval_automouse_current_layer(uint32_t layer_state, uint32_t default_layer_state, uint8_t mouse_layer);

// True when motion must not turn the auto mouse layer on (and an active one should go off).
bool sval_automouse_layer_blocked(uint8_t mode, uint16_t mask, uint32_t layer_state, uint32_t default_layer_state, uint8_t mouse_layer);
