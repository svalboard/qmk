// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"

#ifdef KEY_OVERRIDE_ENABLE

#    include "process_key_override.h"

// Static disabled flag for overrides that aren't enabled
static bool sval_key_override_disabled = false;

// Storage for key override structures
static key_override_t sval_key_overrides[SVAL_KEY_OVERRIDE_ENTRIES];

static void sval_deserialize_key_override(uint16_t index, key_override_t *out) {
    sval_key_override_entry_t entry;
    if (sval_get_key_override(index, &entry) != 0) {
        memset(out, 0, sizeof(*out));
        out->enabled = &sval_key_override_disabled;
        return;
    }

    memset(out, 0, sizeof(*out));
    out->trigger           = entry.trigger;
    out->trigger_mods      = entry.trigger_mods;
    out->layers            = entry.layers;
    out->negative_mod_mask = entry.negative_mod_mask;
    out->suppressed_mods   = entry.suppressed_mods;
    out->replacement       = entry.replacement;
    out->options           = 0;
    out->custom_action     = NULL;
    out->context           = NULL;

    uint8_t opt = entry.options;
    if (opt & sval_ko_enabled) {
        out->enabled = NULL; // NULL means enabled
    } else {
        out->enabled = &sval_key_override_disabled;
    }

    // Parse option bits
    if (opt & sval_ko_option_activation_trigger_down) out->options |= ko_option_activation_trigger_down;
    if (opt & sval_ko_option_activation_required_mod_down) out->options |= ko_option_activation_required_mod_down;
    if (opt & sval_ko_option_activation_negative_mod_up) out->options |= ko_option_activation_negative_mod_up;
    if (opt & sval_ko_option_one_mod) out->options |= ko_option_one_mod;
    if (opt & sval_ko_option_no_reregister_trigger) out->options |= ko_option_no_reregister_trigger;
    if (opt & sval_ko_option_no_unregister_on_other_key_down) out->options |= ko_option_no_unregister_on_other_key_down;
}

void sval_reload_key_override(void) {
    for (size_t i = 0; i < SVAL_KEY_OVERRIDE_ENTRIES; ++i) {
        key_override_t next;
        sval_deserialize_key_override(i, &next);
        if (memcmp(&next, &sval_key_overrides[i], sizeof(next)) == 0) continue;
        // QMK keeps a pointer to the active override and releases its
        // replacement through it later. Deactivate it while the entries are
        // still the ones it pressed, before one of them is rewritten.
        if (key_override_is_enabled()) {
            key_override_off();
            key_override_on();
        }
        memcpy(&sval_key_overrides[i], &next, sizeof(next));
    }
}

// Override the introspection functions
uint16_t key_override_count(void) {
    return SVAL_KEY_OVERRIDE_ENTRIES;
}

const key_override_t *key_override_get(uint16_t key_override_idx) {
    if (key_override_idx >= SVAL_KEY_OVERRIDE_ENTRIES) {
        return NULL;
    }
    return &sval_key_overrides[key_override_idx];
}

#else
// Stubs when KEY_OVERRIDE_ENABLE is not defined
void sval_reload_key_override(void) {}
#endif
