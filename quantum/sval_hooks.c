// Copyright 2026 Svalboard contributors
// SPDX-License-Identifier: GPL-2.0-or-later

/* Weak defaults for the runtime getters in sval_hooks.h. Each returns the
 * compile-time constant QMK would use, so a build that sets
 * RUNTIME_QMK_SETTINGS without overriding a getter behaves as upstream. */

#include "sval_hooks.h"
#include "action.h"
#include "action_tapping.h"

#ifdef RUNTIME_QMK_SETTINGS
__attribute__((weak)) uint16_t get_tap_code_delay(void) {
    return TAP_CODE_DELAY;
}

__attribute__((weak)) uint16_t get_tap_hold_caps_delay(void) {
    return TAP_HOLD_CAPS_DELAY;
}

__attribute__((weak)) uint8_t get_tapping_toggle(void) {
    return TAPPING_TOGGLE;
}

__attribute__((weak)) uint16_t get_oneshot_timeout(void) {
#    ifdef ONESHOT_TIMEOUT
    return ONESHOT_TIMEOUT;
#    else
    return 0;
#    endif
}

__attribute__((weak)) uint8_t get_oneshot_tap_toggle(void) {
#    ifdef ONESHOT_TAP_TOGGLE
    return ONESHOT_TAP_TOGGLE;
#    else
    return 0;
#    endif
}

__attribute__((weak)) uint8_t get_grave_esc_overrides(void) {
    return GRAVE_ESC_OVERRIDES_DEFAULT;
}

__attribute__((weak)) bool get_auto_shift_modifiers(void) {
    return AUTO_SHIFT_MODIFIERS_DEFAULT;
}
#endif

#ifdef LAYER_SWITCH_GET_LAYER_OVERRIDE
__attribute__((weak)) uint8_t layer_switch_get_layer_override(keypos_t key) {
    return UINT8_MAX;
}
#endif
