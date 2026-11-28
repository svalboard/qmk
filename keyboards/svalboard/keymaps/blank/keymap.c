/*
Copyright 2023 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../keymap_support.c"
#include "keycodes.h"
#include "quantum_keycodes.h"
#include QMK_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>
#include "svalboard.h"

// As in the sval keymap, the LEDs show the stored color (set in Keybard) of the
// layer in effect: the highest of the active layers and the default layer.
// No rgblight overlay layers cover it.
layer_state_t default_layer_state_set_user(layer_state_t state) {
  sval_set_active_layer(get_highest_layer(layer_state | state), false);
  return state;
}

layer_state_t layer_state_set_user(layer_state_t state) {
  sval_set_active_layer(get_highest_layer(state | default_layer_state), false);
  return state;
}

enum layer {
    NORMAL,
    NORMAL_HOLD,
    FUNC,
    FUNC_HOLD,
    NAS,
    MBO = MH_AUTO_BUTTONS_LAYER,
};

// Empty keymap - all keys configured via VIA/Sval
const uint16_t PROGMEM keymaps[DYNAMIC_KEYMAP_LAYER_COUNT][MATRIX_ROWS][MATRIX_COLS] = {
};
