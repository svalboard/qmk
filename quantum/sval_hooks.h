// Copyright 2026 Svalboard contributors
// SPDX-License-Identifier: GPL-2.0-or-later

/* Runtime getters for values QMK otherwise fixes at compile time.
 *
 * Each GET_*() below follows QMK's GET_TAPPING_TERM() pattern. Without
 * RUNTIME_QMK_SETTINGS it expands to the compile-time constant, so the code
 * QMK builds is unchanged. With RUNTIME_QMK_SETTINGS (set by a module's
 * post_config.h) it calls the getter; sval_hooks.c holds weak defaults that
 * return the same constants, and the module overrides them with stored
 * values. The constants are then only the reset defaults.
 *
 * These are the only fork-specific declarations the patched core files use.
 * QMK_PATCHES.md (modules/svalboard/core/docs) lists every call site.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

#ifdef __cplusplus
extern "C" {
#endif

// Grave Escape override bits, as QMK Settings stores them (QSID 1).
#define GRAVE_ESC_ALT_OVERRIDE_BIT (1 << 0)
#define GRAVE_ESC_CTRL_OVERRIDE_BIT (1 << 1)
#define GRAVE_ESC_GUI_OVERRIDE_BIT (1 << 2)
#define GRAVE_ESC_SHIFT_OVERRIDE_BIT (1 << 3)

#ifdef GRAVE_ESC_ALT_OVERRIDE
#    define GRAVE_ESC_ALT_OVERRIDE_DEFAULT GRAVE_ESC_ALT_OVERRIDE_BIT
#else
#    define GRAVE_ESC_ALT_OVERRIDE_DEFAULT 0
#endif
#ifdef GRAVE_ESC_CTRL_OVERRIDE
#    define GRAVE_ESC_CTRL_OVERRIDE_DEFAULT GRAVE_ESC_CTRL_OVERRIDE_BIT
#else
#    define GRAVE_ESC_CTRL_OVERRIDE_DEFAULT 0
#endif
#ifdef GRAVE_ESC_GUI_OVERRIDE
#    define GRAVE_ESC_GUI_OVERRIDE_DEFAULT GRAVE_ESC_GUI_OVERRIDE_BIT
#else
#    define GRAVE_ESC_GUI_OVERRIDE_DEFAULT 0
#endif
#ifdef GRAVE_ESC_SHIFT_OVERRIDE
#    define GRAVE_ESC_SHIFT_OVERRIDE_DEFAULT GRAVE_ESC_SHIFT_OVERRIDE_BIT
#else
#    define GRAVE_ESC_SHIFT_OVERRIDE_DEFAULT 0
#endif
#define GRAVE_ESC_OVERRIDES_DEFAULT (GRAVE_ESC_ALT_OVERRIDE_DEFAULT | GRAVE_ESC_CTRL_OVERRIDE_DEFAULT | GRAVE_ESC_GUI_OVERRIDE_DEFAULT | GRAVE_ESC_SHIFT_OVERRIDE_DEFAULT)

#ifdef AUTO_SHIFT_MODIFIERS
#    define AUTO_SHIFT_MODIFIERS_DEFAULT true
#else
#    define AUTO_SHIFT_MODIFIERS_DEFAULT false
#endif

#ifdef RUNTIME_QMK_SETTINGS
uint16_t get_tap_code_delay(void);       // TAP_CODE_DELAY
uint16_t get_tap_hold_caps_delay(void);  // TAP_HOLD_CAPS_DELAY
uint8_t  get_tapping_toggle(void);       // TAPPING_TOGGLE
uint16_t get_oneshot_timeout(void);      // ONESHOT_TIMEOUT; 0 = no timeout
uint8_t  get_oneshot_tap_toggle(void);   // ONESHOT_TAP_TOGGLE; 0 or 1 = no locking
uint8_t  get_grave_esc_overrides(void);  // GRAVE_ESC_*_OVERRIDE, as the bits above
bool     get_auto_shift_modifiers(void); // AUTO_SHIFT_MODIFIERS
#    define GET_TAP_CODE_DELAY() get_tap_code_delay()
#    define GET_TAP_HOLD_CAPS_DELAY() get_tap_hold_caps_delay()
#    define GET_TAPPING_TOGGLE() get_tapping_toggle()
#    define GET_ONESHOT_TIMEOUT() get_oneshot_timeout()
#    define GET_ONESHOT_TAP_TOGGLE() get_oneshot_tap_toggle()
#    define GET_GRAVE_ESC_OVERRIDES() get_grave_esc_overrides()
#    define GET_AUTO_SHIFT_MODIFIERS() get_auto_shift_modifiers()
#else
#    define GET_TAP_CODE_DELAY() (TAP_CODE_DELAY)
#    define GET_TAP_HOLD_CAPS_DELAY() (TAP_HOLD_CAPS_DELAY)
#    define GET_TAPPING_TOGGLE() (TAPPING_TOGGLE)
#    define GET_ONESHOT_TIMEOUT() (ONESHOT_TIMEOUT)
#    define GET_ONESHOT_TAP_TOGGLE() (ONESHOT_TAP_TOGGLE)
#    define GET_GRAVE_ESC_OVERRIDES() (GRAVE_ESC_OVERRIDES_DEFAULT)
#    define GET_AUTO_SHIFT_MODIFIERS() (AUTO_SHIFT_MODIFIERS_DEFAULT)
#endif

#ifdef LAYER_SWITCH_GET_LAYER_OVERRIDE
/* Called first by layer_switch_get_layer(). Return the layer that supplies
 * the key's action, or UINT8_MAX for QMK's normal lookup. The weak default
 * returns UINT8_MAX. */
uint8_t layer_switch_get_layer_override(keypos_t key);
#endif

#ifdef __cplusplus
}
#endif
