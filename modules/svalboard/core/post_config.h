// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Include generated config from sval.json (defines SVAL_*_ENTRIES)
#include "sval_config.h"

// Core hook switches (quantum/sval_hooks.h)
#include "sval_runtime_settings.h"

// Defaults for features not in sval.json (0 = disabled)
#ifndef SVAL_TAP_DANCE_ENTRIES
#    define SVAL_TAP_DANCE_ENTRIES 0
#endif

#ifndef SVAL_COMBO_ENTRIES
#    define SVAL_COMBO_ENTRIES 0
#endif

#ifndef SVAL_KEY_OVERRIDE_ENTRIES
#    define SVAL_KEY_OVERRIDE_ENTRIES 0
#endif

#ifndef SVAL_ALT_REPEAT_KEY_ENTRIES
#    define SVAL_ALT_REPEAT_KEY_ENTRIES 0
#endif

#ifndef SVAL_LEADER_ENTRIES
#    define SVAL_LEADER_ENTRIES 0
#endif

// USB serial. Hosts detect a Sval board by the "sval:" magic in the serial string
// (sval-gui util.py), so when a build drops the fixed SERIAL_NUMBER literal in favour
// of the chip's hardware ID, the magic is carried over as a prefix. The result is
// "sval:<hardware id>": still matched by every existing host, but unique per board and
// unchanged by a reflash, so a browser's WebHID grant survives a firmware update.
#ifndef SERIAL_NUMBER_PREFIX
#    define SERIAL_NUMBER_PREFIX "sval:"
#endif

// Route QMK's tap-hold decisions through the per-key callbacks implemented in
// sval_qmk_settings.c so the values stored via QMK Settings (tapping term,
// permissive hold, hold-on-other-key-press, quick tap term) take effect at
// runtime. These must be C defines, not rules.mk variables: without them QMK
// compiles the callbacks out and uses the compile-time constants instead.
#ifndef TAPPING_TERM_PER_KEY
#    define TAPPING_TERM_PER_KEY
#endif
#ifndef PERMISSIVE_HOLD_PER_KEY
#    define PERMISSIVE_HOLD_PER_KEY
#endif
#ifndef HOLD_ON_OTHER_KEY_PRESS_PER_KEY
#    define HOLD_ON_OTHER_KEY_PRESS_PER_KEY
#endif
#ifndef QUICK_TAP_TERM_PER_KEY
#    define QUICK_TAP_TERM_PER_KEY
#endif

// Compile in the remaining Tap-Hold features whose toggles QMK Settings
// exposes (Chordal Hold, Flow Tap, Retro Tapping). Each decision is routed
// through a callback in sval_qmk_settings.c that honours the stored setting;
// with the setting off, behaviour is the same as the feature not being built.
#ifndef CHORDAL_HOLD
#    define CHORDAL_HOLD
#endif
#ifndef FLOW_TAP_TERM
#    define FLOW_TAP_TERM 0 // placeholder: the runtime value comes from get_flow_tap_term()
#endif
#ifndef RETRO_TAPPING_PER_KEY
#    define RETRO_TAPPING_PER_KEY
#endif

// Auto Shift starts off until enabled via QMK Settings (QSID 3 bit 0). Its
// repeat options: sval_runtime_settings.h.
#ifndef AUTO_SHIFT_DISABLED_AT_STARTUP
#    define AUTO_SHIFT_DISABLED_AT_STARTUP
#endif

// Per-combo and global combo terms come from get_combo_term() in sval_combo.c.
#ifndef COMBO_TERM_PER_COMBO
#    define COMBO_TERM_PER_COMBO
#endif

// QMK settings storage size (sval_qmk_settings_t)
#define SVAL_QMK_SETTINGS_SIZE 44

// Leader key defaults written by a settings reset, and to a board that stored a
// timeout of 0. 300 ms is QMK's own LEADER_TIMEOUT default. Per-key timing
// restarts the 300 ms after each key, so a sequence can be any length as long
// as no gap between keys is longer than that; most QMK keymaps that set a
// leader timeout also turn it on.
#ifndef SVAL_DEFAULT_LEADER_TIMEOUT
#    define SVAL_DEFAULT_LEADER_TIMEOUT 300
#endif
#ifndef SVAL_DEFAULT_LEADER_PER_KEY_TIMING
#    define SVAL_DEFAULT_LEADER_PER_KEY_TIMING 1
#endif

// Dynamic leader timeout - Sval controls this variable
#if defined(LEADER_ENABLE) && !defined(__ASSEMBLER__)
#    include <stdint.h>
extern uint16_t sval_leader_timeout;
#    define LEADER_TIMEOUT (sval_leader_timeout)
#endif

// Defaults for dynamic keymap counts (needed for label storage calculation)
#ifndef DYNAMIC_KEYMAP_LAYER_COUNT
#    define DYNAMIC_KEYMAP_LAYER_COUNT 4
#endif

#ifndef DYNAMIC_KEYMAP_MACRO_COUNT
#    define DYNAMIC_KEYMAP_MACRO_COUNT 16
#endif

// Total size: tap_dance*10 + combo*12 + key_override*12 + alt_repeat*6 + leader*14 + magic(6) + qmk_settings(44) + fragments(21) + labels_v2(td*16 + macro*16 + layer*16)
#define SVAL_EEPROM_SIZE_CALC ((SVAL_TAP_DANCE_ENTRIES * 10) + (SVAL_COMBO_ENTRIES * 12) + (SVAL_KEY_OVERRIDE_ENTRIES * 12) + (SVAL_ALT_REPEAT_KEY_ENTRIES * 6) + (SVAL_LEADER_ENTRIES * 14) + 6 + SVAL_QMK_SETTINGS_SIZE + 21 + (SVAL_TAP_DANCE_ENTRIES * 16) + (DYNAMIC_KEYMAP_MACRO_COUNT * 16) + (DYNAMIC_KEYMAP_LAYER_COUNT * 16) + 1)

#ifndef EECONFIG_KB_DATA_SIZE
#    define EECONFIG_KB_DATA_SIZE SVAL_EEPROM_SIZE_CALC
#endif

// Combo names (label type 3) live at the very end of the logical EEPROM, after
// the macro buffer, not in the Sval data block above. Growing that block would
// move the keymap, the macros and VIA's own data, and change the Sval storage
// stamp, so every board would lose its settings. Here only the macro buffer's
// unused tail gets shorter; everything already stored stays where it is. The
// region has its own header (sval.c), so a board that never had it reads as
// having no combo names.
#if SVAL_COMBO_ENTRIES > 0 && defined(WEAR_LEVELING_LOGICAL_SIZE) && !defined(SVAL_NO_COMBO_LABELS)
#    define SVAL_COMBO_LABELS
#    define SVAL_COMBO_LABEL_HEADER_SIZE 8
#    define SVAL_COMBO_LABEL_REGION_SIZE (SVAL_COMBO_LABEL_HEADER_SIZE + (SVAL_COMBO_ENTRIES * 16))
#    define SVAL_COMBO_LABEL_REGION_ADDR ((WEAR_LEVELING_LOGICAL_SIZE) - (SVAL_COMBO_LABEL_REGION_SIZE))
#    ifdef DYNAMIC_KEYMAP_EEPROM_MAX_ADDR
#        error "Combo names take the end of the EEPROM; leave DYNAMIC_KEYMAP_EEPROM_MAX_ADDR to the Sval module or define SVAL_NO_COMBO_LABELS"
#    endif
#    define DYNAMIC_KEYMAP_EEPROM_MAX_ADDR (SVAL_COMBO_LABEL_REGION_ADDR - 1)
// VIA's layout stamp keeps using the buffer's original end (nvm_dynamic_keymap.c),
// so this doesn't reset keymaps and macros.
#    define DYNAMIC_KEYMAP_STAMP_MAX_ADDR ((WEAR_LEVELING_LOGICAL_SIZE)-1)
#endif
