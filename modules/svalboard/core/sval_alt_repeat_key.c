// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"

#ifdef REPEAT_KEY_ENABLE

// An entry as the lookup uses it: each keycode reduced to its basic (tap)
// keycode, with the mods of a modded keycode moved into its required mods.
// Matching follows the Vial firmware (quantum/vial.c), so entries made there
// behave the same here.
typedef struct {
    uint16_t keycode;
    uint16_t alt_keycode;
    uint8_t  required_mods;
    uint8_t  alt_required_mods;
    uint8_t  allowed_mods;
    uint8_t  options;
} sval_alt_repeat_t;

static sval_alt_repeat_t sval_alt_repeat_entries[SVAL_ALT_REPEAT_KEY_ENTRIES];

// The 5-bit mods of a keycode (bit 4 = right hand) as 8-bit mods.
static uint8_t unpack_mods5(uint8_t mods5) {
    return (mods5 & 0x10) != 0 ? (uint8_t)(mods5 << 4) : mods5;
}

// 8-bit mods as the 5-bit mods of a keycode. Mixed hands become right hand,
// as QMK's own alt repeat lookup does (quantum/repeat_key.c).
static uint8_t pack_mods5(uint8_t mods) {
    return ((mods & 0xf0) ? 0x10 : 0) | (((mods >> 4) | mods) & 0xf);
}

// Reduce a keycode to its basic keycode: a modded keycode gives up its mods
// into *mods, and a mod-tap or layer-tap key gives its tap keycode.
static uint16_t normalize_keycode(uint16_t keycode, uint8_t *mods) {
    switch (keycode) {
        case QK_MODS ... QK_MODS_MAX:
            *mods |= unpack_mods5(QK_MODS_GET_MODS(keycode));
            keycode = QK_MODS_GET_BASIC_KEYCODE(keycode);
            break;
        case QK_MOD_TAP ... QK_MOD_TAP_MAX:
            keycode = QK_MOD_TAP_GET_TAP_KEYCODE(keycode);
            break;
        case QK_LAYER_TAP ... QK_LAYER_TAP_MAX:
            keycode = QK_LAYER_TAP_GET_TAP_KEYCODE(keycode);
            break;
    }
    return keycode;
}

// A basic keycode with mods, as a keycode.
static uint16_t with_mods(uint8_t mods, uint16_t keycode) {
    return mods ? (uint16_t)(pack_mods5(mods) << 8) | keycode : keycode;
}

void sval_reload_alt_repeat_key(void) {
    memset(sval_alt_repeat_entries, 0, sizeof(sval_alt_repeat_entries));
    for (size_t i = 0; i < SVAL_ALT_REPEAT_KEY_ENTRIES; ++i) {
        sval_alt_repeat_key_entry_t entry;
        if (sval_get_alt_repeat_key(i, &entry) != 0) continue;
        sval_alt_repeat_t *out = &sval_alt_repeat_entries[i];
        out->keycode           = normalize_keycode(entry.keycode, &out->required_mods);
        out->alt_keycode       = normalize_keycode(entry.alt_keycode, &out->alt_required_mods);
        out->allowed_mods      = entry.allowed_mods;
        out->options           = entry.options;
    }
}

// The held mods match when every required mod is held and every other held
// mod is allowed. Allowed mods are extra mods that may be held, not mods that
// must be.
static bool mods_match(uint8_t mods, uint8_t required_mods, uint8_t allowed_mods, uint8_t options) {
    allowed_mods |= required_mods;
    if (options & sval_ark_option_ignore_mod_handedness) {
        mods          = (mods & 0xf) | (mods >> 4);
        required_mods = (required_mods & 0xf) | (required_mods >> 4);
        allowed_mods  = (allowed_mods & 0xf) | (allowed_mods >> 4);
    }
    return (mods & required_mods) == required_mods && (mods & ~allowed_mods) == 0;
}

// QMK's alt repeat callback. Of the enabled entries that match the last key
// and mods, in either direction for a bidirectional entry, the one with the
// most required mods wins. With no match, an entry set as the default alt key
// gives its alt key for any last key, if the held mods are allowed. Otherwise
// QMK's built-in alt keys apply.
uint16_t get_alt_repeat_key_keycode_user(uint16_t keycode, uint8_t mods) {
    uint16_t alt_keycode = KC_TRANSPARENT;
    int8_t   best_fit    = -1;

    keycode = normalize_keycode(keycode, &mods);

    for (size_t i = 0; i < SVAL_ALT_REPEAT_KEY_ENTRIES; ++i) {
        const sval_alt_repeat_t *entry   = &sval_alt_repeat_entries[i];
        const uint8_t            options = entry->options;
        if (!(options & sval_ark_enabled)) continue;

        if (entry->keycode == keycode && mods_match(mods, entry->required_mods, entry->allowed_mods, options)) {
            const int8_t fit = bitpop(entry->required_mods);
            if (fit > best_fit) {
                alt_keycode = with_mods(entry->alt_required_mods, entry->alt_keycode);
                best_fit    = fit;
            }
        }

        if (entry->alt_keycode == keycode && (options & sval_ark_option_bidirectional) && mods_match(mods, entry->alt_required_mods, entry->allowed_mods, options)) {
            const int8_t fit = bitpop(entry->alt_required_mods);
            if (fit > best_fit) {
                alt_keycode = with_mods(entry->required_mods, entry->keycode);
                best_fit    = fit;
            }
        }

        if ((options & sval_ark_option_default_to_alt) && best_fit == -1 && alt_keycode == KC_TRANSPARENT && mods_match(mods, 0, entry->allowed_mods, options)) {
            alt_keycode = with_mods(entry->alt_required_mods, entry->alt_keycode);
        }
    }

    return alt_keycode;
}

#else
// Stubs when REPEAT_KEY_ENABLE is not defined
void sval_reload_alt_repeat_key(void) {}
#endif
