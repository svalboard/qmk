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
#pragma once

#include QMK_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>
#include "svalboard.h"

// VIA's customKeycodes maps to QK_USER range (0x7E40+), not QK_KB (0x7E00+).
// TODO: Investigate using QK_KB range for keyboard-level keycodes in the future.
enum my_keycodes {
    SV_LEFT_DPI_INC = QK_USER_0,
    SV_LEFT_DPI_DEC,
    SV_RIGHT_DPI_INC,
    SV_RIGHT_DPI_DEC,
    SV_LEFT_SCROLL_TOGGLE,
    SV_RIGHT_SCROLL_TOGGLE,
    SV_RECALIBRATE_POINTER,
    SV_MH_CHANGE_TIMEOUTS,
    SV_CAPS_WORD,
    SV_AXIS_SCROLL_LOCK,
    SV_TOGGLE_23_67,
    SV_TOGGLE_45_67,
    SV_SNIPER_2,
    SV_SNIPER_3,
    SV_SNIPER_5,
    SV_SCROLL_HOLD,
    SV_SCROLL_TOGGLE,
    SV_OUTPUT_STATUS,
    SV_TOGGLE_AUTOMOUSE,
    SV_TURBO_SCAN,
    SV_NATURAL_SCROLL_TOGGLE,
    SV_SNIPER_2_TG,
    SV_SNIPER_3_TG,
    SV_SNIPER_5_TG,
    SV_BOOST_2,         // pos 24 — multiplies x/y AND h/v, like sniper divides both
    SV_BOOST_3,         // pos 25
    SV_BOOST_5,         // pos 26
    SV_BOOST_2_TG,      // pos 27
    SV_BOOST_3_TG,      // pos 28
    SV_BOOST_5_TG,      // pos 29
    SV_VELOCITY_SCROLL, // pos 30 — hold: pointer offset sets a scroll speed (velocity_scroll.h)
    SV_VELOCITY_SCROLL_TOGGLE, // pos 31 — tap on, tap off; otherwise as SV_VELOCITY_SCROLL. The GUI cap is QK_USER_31 inclusive: this is the last slot
    // First keycode a custom keymap may define. SAFE_RANGE (QK_USER_0) is
    // not safe here: QK_USER_0..QK_USER_31 are the Svalboard keycodes above.
    // Start custom keymap keycodes at SV_SAFE_RANGE, for example
    // `enum { MY_KEY = SV_SAFE_RANGE, MY_OTHER_KEY };`.
    SV_SAFE_RANGE = QK_USER_31 + 1,
};

#define MH_AUTO_BUTTONS_LAYER (DYNAMIC_KEYMAP_LAYER_COUNT - 1)

// Every SV_ keycode keeps its QK_USER_n value: sval.json's customKeycodes
// (keymaps/*/sval.json) address them by position n.
#define SV_KC_AT(kc, n) _Static_assert((int)(kc) == (int)QK_USER_##n, #kc " must be QK_USER_" #n " (sval.json customKeycodes position " #n ")")
SV_KC_AT(SV_LEFT_DPI_INC, 0);
SV_KC_AT(SV_LEFT_DPI_DEC, 1);
SV_KC_AT(SV_RIGHT_DPI_INC, 2);
SV_KC_AT(SV_RIGHT_DPI_DEC, 3);
SV_KC_AT(SV_LEFT_SCROLL_TOGGLE, 4);
SV_KC_AT(SV_RIGHT_SCROLL_TOGGLE, 5);
SV_KC_AT(SV_RECALIBRATE_POINTER, 6);
SV_KC_AT(SV_MH_CHANGE_TIMEOUTS, 7);
SV_KC_AT(SV_CAPS_WORD, 8);
SV_KC_AT(SV_AXIS_SCROLL_LOCK, 9);
SV_KC_AT(SV_TOGGLE_23_67, 10);
SV_KC_AT(SV_TOGGLE_45_67, 11);
SV_KC_AT(SV_SNIPER_2, 12);
SV_KC_AT(SV_SNIPER_3, 13);
SV_KC_AT(SV_SNIPER_5, 14);
SV_KC_AT(SV_SCROLL_HOLD, 15);
SV_KC_AT(SV_SCROLL_TOGGLE, 16);
SV_KC_AT(SV_OUTPUT_STATUS, 17);
SV_KC_AT(SV_TOGGLE_AUTOMOUSE, 18);
SV_KC_AT(SV_TURBO_SCAN, 19);
SV_KC_AT(SV_NATURAL_SCROLL_TOGGLE, 20);
SV_KC_AT(SV_SNIPER_2_TG, 21);
SV_KC_AT(SV_SNIPER_3_TG, 22);
SV_KC_AT(SV_SNIPER_5_TG, 23);
SV_KC_AT(SV_BOOST_2, 24);
SV_KC_AT(SV_BOOST_3, 25);
SV_KC_AT(SV_BOOST_5, 26);
SV_KC_AT(SV_BOOST_2_TG, 27);
SV_KC_AT(SV_BOOST_3_TG, 28);
SV_KC_AT(SV_BOOST_5_TG, 29);
SV_KC_AT(SV_VELOCITY_SCROLL, 30);
SV_KC_AT(SV_VELOCITY_SCROLL_TOGGLE, 31);
_Static_assert((int)SV_SAFE_RANGE > (int)SV_VELOCITY_SCROLL_TOGGLE, "SV_SAFE_RANGE must be past the last Svalboard keycode");

// Velocity Scroll is on (its key held, its toggle on, or the touchpad dev command).
bool velocity_scroll_active(void);
// Dev builds (SVAL_TOUCH_DEV, raw HID 0xFC op 0x42 action 2): hold it with no
// key, for keyless test boards. Expires unless repeated within 500 ms.
void velocity_scroll_dev_hold(bool on);
