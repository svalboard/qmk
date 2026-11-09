// Copyright 2026 Svalboard contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Switches for the core hooks in quantum/sval_hooks.h. Included by
// post_config.h, and by the runtime tests in tests/sval_runtime_tap so they
// build QMK exactly as the module does.

#pragma once

// QMK Settings values (tap code delay, tapping toggle, one-shot timeout and
// tap toggle, Grave Escape overrides, Auto Shift modifiers) come from the
// get_*() getters in sval_qmk_settings.c instead of compile-time constants.
#define RUNTIME_QMK_SETTINGS

// The host's context layer is consulted first in layer_switch_get_layer()
// (layer_switch_get_layer_override() in sval_context_layer.c).
#define LAYER_SWITCH_GET_LAYER_OVERRIDE

// Combos, tap dances and key overrides come from Sval's EEPROM tables, not
// keymap arrays, so QMK's keymap-array introspection is left out.
#define NO_KEYMAP_FEATURE_TABLES

// Auto Shift's repeat options (QSID 3 bits 5 and 6) go through QMK's per-key
// hooks, get_auto_shift_repeat() and get_auto_shift_no_auto_repeat() in
// sval_auto_shift.c.
#ifndef AUTO_SHIFT_REPEAT_PER_KEY
#    define AUTO_SHIFT_REPEAT_PER_KEY
#endif
#ifndef AUTO_SHIFT_NO_AUTO_REPEAT_PER_KEY
#    define AUTO_SHIFT_NO_AUTO_REPEAT_PER_KEY
#endif

// QMK compiles its one-shot timeout, one-shot tap toggle and Grave Escape
// override code only when these are defined. The runtime getters supply the
// values; the numbers here are only the reset defaults.
#ifndef ONESHOT_TIMEOUT
#    define ONESHOT_TIMEOUT 5000
#elif ONESHOT_TIMEOUT <= 0
#    error "ONESHOT_TIMEOUT is the Sval reset default and must be above 0; QMK Settings sets 0 at run time"
#endif
#ifndef ONESHOT_TAP_TOGGLE
#    define ONESHOT_TAP_TOGGLE 5
#elif ONESHOT_TAP_TOGGLE <= 1
#    error "ONESHOT_TAP_TOGGLE is the Sval reset default and must be above 1; QMK Settings sets 0 or 1 at run time"
#endif
#if defined(TAPPING_TOGGLE) && TAPPING_TOGGLE == 0
#    error "TAPPING_TOGGLE is the Sval reset default and must be above 0"
#endif
#ifndef GRAVE_ESC_ALT_OVERRIDE
#    define GRAVE_ESC_ALT_OVERRIDE
#endif
#ifndef GRAVE_ESC_CTRL_OVERRIDE
#    define GRAVE_ESC_CTRL_OVERRIDE
#endif
#ifndef GRAVE_ESC_GUI_OVERRIDE
#    define GRAVE_ESC_GUI_OVERRIDE
#endif
#ifndef GRAVE_ESC_SHIFT_OVERRIDE
#    define GRAVE_ESC_SHIFT_OVERRIDE
#endif
