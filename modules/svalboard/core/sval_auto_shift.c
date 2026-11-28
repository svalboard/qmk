// Copyright 2026 Svalboard contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The Auto Shift options in QMK Settings (QSID 3) other than on and off,
// applied through QMK's own hooks and the get_auto_shift_modifiers() getter
// (quantum/sval_hooks.h). Kept apart from sval_qmk_settings.c so the QMK unit
// tests in tests/sval_runtime_tap/auto_shift can build this file as it ships.

#include "quantum.h"
#include "sval_qmk_settings.h"

bool get_auto_shift_modifiers(void) {
    return sval_auto_shift() & (1 << AUTO_SHIFT_MODIFIERS_BIT);
}

#ifdef AUTO_SHIFT_ENABLE
// QMK's get_auto_shifted_key() with the no-alpha, no-numeric and no-special
// bits applied. A category whose bit is set falls through to
// get_custom_auto_shifted_key(), as a key outside every category does.
bool get_auto_shifted_key(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
#    ifndef NO_AUTO_SHIFT_ALPHA
        case AUTO_SHIFT_ALPHA:
            if (sval_auto_shift() & (1 << AUTO_SHIFT_NO_ALPHA_BIT)) break;
            return true;
#    endif
#    ifndef NO_AUTO_SHIFT_NUMERIC
        case AUTO_SHIFT_NUMERIC:
            if (sval_auto_shift() & (1 << AUTO_SHIFT_NO_NUMERIC_BIT)) break;
            return true;
#    endif
#    ifndef NO_AUTO_SHIFT_SPECIAL
#        ifndef NO_AUTO_SHIFT_TAB
        case KC_TAB:
#        endif
#        ifndef NO_AUTO_SHIFT_SYMBOLS
        case AUTO_SHIFT_SYMBOLS:
#        endif
            if (sval_auto_shift() & (1 << AUTO_SHIFT_NO_SPECIAL_BIT)) break;
            return true;
#    endif
#    ifdef AUTO_SHIFT_ENTER
        case KC_ENT:
            return true;
#    endif
    }
    return get_custom_auto_shifted_key(keycode, record);
}

// QMK calls these per key (AUTO_SHIFT_REPEAT_PER_KEY and
// AUTO_SHIFT_NO_AUTO_REPEAT_PER_KEY, post_config.h).
bool get_auto_shift_repeat(uint16_t keycode, keyrecord_t *record) {
    return (sval_auto_shift() & (1 << AUTO_SHIFT_REPEAT_BIT)) != 0;
}

bool get_auto_shift_no_auto_repeat(uint16_t keycode, keyrecord_t *record) {
    return (sval_auto_shift() & (1 << AUTO_SHIFT_NO_AUTO_REPEAT_BIT)) != 0;
}
#endif
