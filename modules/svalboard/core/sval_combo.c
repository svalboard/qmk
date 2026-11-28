// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"

#ifdef COMBO_ENABLE

#    include "process_combo.h"

// Bit mask for enabled flag in custom_combo_term
#    define SVAL_COMBO_ENABLED_BIT 0x8000
// Mask for timing value (bits 0-14)
#    define SVAL_COMBO_TIMING_MASK 0x7FFF

// Storage for combo key sequences (4 keys + COMBO_END terminator)
static uint16_t sval_combo_keys[SVAL_COMBO_ENTRIES][5];

// Storage for combo structures
static combo_t sval_combos[SVAL_COMBO_ENTRIES];

// Storage for custom combo terms (0 = use global default)
static uint16_t sval_combo_terms[SVAL_COMBO_ENTRIES];

// Track which combos are enabled
static bool sval_combo_enabled[SVAL_COMBO_ENTRIES];

#    ifdef EXTRA_SHORT_COMBOS
#        define SVAL_COMBO_IS_ACTIVE(combo) (((combo)->state & 0x80) != 0)
#    else
#        define SVAL_COMBO_IS_ACTIVE(combo) ((combo)->active)
#    endif

// Release a held combo's output as release_combo() in process_combo.c (static
// there) does: the same combo event its press went out as.
static void sval_release_combo(uint16_t index, combo_t *combo) {
    if (combo->keycode) {
        keyrecord_t record = {
            .event   = MAKE_COMBOEVENT(false),
            .keycode = combo->keycode,
        };
#    ifndef NO_ACTION_TAPPING
        action_tapping_process(record);
#    else
        process_record(&record);
#    endif
    } else {
        process_combo_event(index, false);
    }
}

// Load every combo from storage. A combo whose keys and output are unchanged
// keeps its live state, so one held down while any combo is written still
// releases its output when its keys come up. One that changes while it is
// held releases its old output first.
void sval_reload_combo(void) {
    for (size_t i = 0; i < SVAL_COMBO_ENTRIES; ++i) {
        // All keys = COMBO_END unless an enabled entry sets them
        uint16_t keys[5] = {0};
        uint16_t keycode = KC_NO;
        uint16_t term    = 0;
        bool     enabled = false;

        // Load from EEPROM
        sval_combo_entry_t entry;
        if (sval_get_combo(i, &entry) == 0) {
            // Check if enabled (bit 15 of custom_combo_term)
            enabled = (entry.custom_combo_term & SVAL_COMBO_ENABLED_BIT) != 0;

            if (enabled) {
                memcpy(keys, entry.input, sizeof(entry.input));
                // Ensure null termination
                keys[4] = COMBO_END;
                keycode = entry.output;

                // Extract custom timing (bits 0-14), 0 means use global default
                term = entry.custom_combo_term & SVAL_COMBO_TIMING_MASK;
            }
            // Disabled combo: empty key sequence, KC_NO
        }

        combo_t *combo = &sval_combos[i];
        bool     same  = combo->keys == sval_combo_keys[i] && combo->keycode == keycode && memcmp(sval_combo_keys[i], keys, sizeof(keys)) == 0;
        if (!same) {
            if (combo->keys && SVAL_COMBO_IS_ACTIVE(combo)) sval_release_combo(i, combo);
            memcpy(sval_combo_keys[i], keys, sizeof(keys));
            memset(combo, 0, sizeof(*combo));
            combo->keys    = sval_combo_keys[i];
            combo->keycode = keycode;
        }
        sval_combo_terms[i]   = term;
        sval_combo_enabled[i] = enabled;
    }
}

// Override the introspection functions
uint16_t combo_count(void) {
    return SVAL_COMBO_ENTRIES;
}

combo_t *combo_get(uint16_t combo_idx) {
    if (combo_idx >= SVAL_COMBO_ENTRIES) {
        return NULL;
    }
    return &sval_combos[combo_idx];
}

// User hook: override this for custom per-combo timing logic
// Return 0 to use Sval's setting, or a positive value to override
__attribute__((weak)) uint16_t get_combo_term_sval(uint16_t combo_idx, combo_t *combo) {
    return 0; // Default: use Sval's setting
}

// Sval owns this function - user hook is checked FIRST
uint16_t get_combo_term(uint16_t combo_idx, combo_t *combo) {
    // User hook gets first priority
    uint16_t user_term = get_combo_term_sval(combo_idx, combo);
    if (user_term > 0) {
        return user_term;
    }

    // Then check for per-combo custom timing from Sval
    if (combo_idx < SVAL_COMBO_ENTRIES && sval_combo_terms[combo_idx] > 0) {
        return sval_combo_terms[combo_idx];
    }

    // Fall back to Sval's global setting
    return sval_get_combo_term();
}

#else
// Stubs when COMBO_ENABLE is not defined
void sval_reload_combo(void) {}
#endif
