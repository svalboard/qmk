// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialize QMK settings (load from EEPROM and apply)
void sval_qmk_settings_init(void);

// Query supported QSIDs greater than qsid_gt
void sval_qmk_settings_query(uint16_t qsid_gt, uint8_t *buffer, uint8_t length);

// Get a setting value (returns 0 on success)
int sval_qmk_settings_get(uint16_t qsid, uint8_t *buffer, uint8_t length);

// Set a setting value (returns 0 on success)
int sval_qmk_settings_set(uint16_t qsid, const uint8_t *data, uint8_t length);

// Reset all settings to defaults
void sval_qmk_settings_reset(void);

// Getters for global settings (used by other sval modules as fallback)
uint16_t sval_get_tapping_term(void);
uint16_t sval_get_combo_term(void);
uint16_t sval_get_leader_timeout(void);
bool     sval_get_leader_per_key_timing(void);

// The stored Auto Shift bits (QSID 3), as Keybard writes them. Bit 0 (on)
// is applied in sval_qmk_settings.c, the rest in sval_auto_shift.c.
#define AUTO_SHIFT_MODIFIERS_BIT 1
#define AUTO_SHIFT_NO_SPECIAL_BIT 2
#define AUTO_SHIFT_NO_NUMERIC_BIT 3
#define AUTO_SHIFT_NO_ALPHA_BIT 4
#define AUTO_SHIFT_REPEAT_BIT 5
#define AUTO_SHIFT_NO_AUTO_REPEAT_BIT 6
uint8_t sval_auto_shift(void);

// The runtime values of QMK's TAP_CODE_DELAY, TAP_HOLD_CAPS_DELAY,
// TAPPING_TOGGLE, ONESHOT_TIMEOUT, ONESHOT_TAP_TOGGLE, GRAVE_ESC_*_OVERRIDE
// and AUTO_SHIFT_MODIFIERS are the get_*() getters declared in
// quantum/sval_hooks.h.

// One-shot keys (QMK settings 5 and 6), also reached through Sval commands 0x09/0x0A
void sval_qmk_settings_set_one_shot(uint16_t timeout, uint8_t tap_toggle);
