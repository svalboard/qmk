// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later

// App rules on the board (app_rules.h): the rules record, the foreground app
// from the host, and how they reach key lookup, the lights and Auto Mouse.
// Only the half with USB runs them: it owns the layer state. The other half
// has no layer state of its own; its lights follow the half with USB through
// the RGB split sync, so it shows what the rules do too.

#include <string.h>
#include QMK_KEYBOARD_H
#include "svalboard.h"
#include "sval.h"
#include "app_rules.h"
#include "app_rules_kb.h"
#include "dynamic_keymap.h"
#include "keymaps/keymap_support.h"
#if VIA_ENABLE
#    include "settings_area.h"
_Static_assert(SVAL_APP_RULES_RECORD_BYTES <= SVAL_SET_APP_RULES_CAP, "the app rules record fits its settings area entry");
#endif

static uint8_t           rules[SVAL_APP_RULES_RECORD_BYTES];   // the rules the board runs
static uint8_t           staging[SVAL_APP_RULES_RECORD_BYTES]; // a host's edit, until COMMIT
static sval_app_engine_t engine;
static bool              engine_ready;
static bool              lookup_shaped; // the rules changed key lookup at the last update

static uint8_t layer_count(void) {
    uint8_t n = dynamic_keymap_get_layer_count();
    return n > 16 ? 16 : n;
}

static void engine_init_once(void) {
    if (!engine_ready) {
        sval_app_engine_init(&engine);
        sval_app_engine_keep(&engine, (uint16_t)(1u << MH_AUTO_BUTTONS_LAYER)); // Auto Mouse owns it
        engine_ready = true;
    }
}

void sval_app_rules_load(void) {
    engine_init_once();
    memset(rules, 0, sizeof(rules));
#if VIA_ENABLE
    uint16_t len = 0;
    settings_area_get(SVAL_SET_APP_RULES, rules, sizeof(rules), &len);
    // Format 1 (pre-release builds): upgrade it; it is saved as format 2 at the next COMMIT.
    if (rules[0] == 1) {
        memcpy(staging, rules, sizeof(rules));
        if (!sval_app_rules_upgrade(staging, len < sizeof(staging) ? len : sizeof(staging), rules)) memset(rules, 0, sizeof(rules));
    }
#endif
    if (!sval_app_rules_valid(rules)) memset(rules, 0, sizeof(rules));
    sval_app_rules_tidy(rules);
    memcpy(staging, rules, sizeof(rules));
}

uint32_t sval_app_rules_layer_view(uint32_t state) {
    return lookup_shaped ? sval_app_engine_layers(&engine, state) : state;
}

// The lights show the highest layer on, as the keymap's layer_state_set_user
// does, but of the layers the rules leave on.
static void refresh_lights(uint32_t state) {
    uint32_t view = sval_app_rules_layer_view(state);
#ifdef RGBLIGHT_LAYERS
    for (int i = 1; i < RGBLIGHT_LAYERS; ++i) rgblight_set_layer_state(i, (view >> i) & 1);
#endif
    sval_set_active_layer(get_highest_layer(view), false);
}

layer_state_t layer_state_set_kb(layer_state_t state) {
    state = layer_state_set_user(state);
    if (lookup_shaped) refresh_lights(state);
    return state;
}

static void update(void) {
    engine_init_once();
    uint32_t set;
    bool     was = lookup_shaped;
    if (sval_app_engine_update(&engine, rules, layer_count(), timer_read32(), default_layer_state, &set)) default_layer_set(set); // runtime only, never saved
    lookup_shaped = sval_app_engine_shapes_lookup(&engine);
    static uint32_t last_view;
    uint32_t        view = sval_app_rules_layer_view(layer_state);
    if (lookup_shaped != was || view != last_view) refresh_lights(layer_state);
    last_view = view;
}

void sval_app_rules_task(void) {
    if (!is_keyboard_master()) return;
    static uint32_t last;
    if (timer_elapsed32(last) < 50) return; // expiry needs no finer step
    last = timer_read32();
    update();
}

uint8_t sval_app_rules_lookup(keypos_t key) {
    if (!lookup_shaped) return UINT8_MAX;
    uint32_t candidates = sval_app_engine_layers(&engine, layer_state) | default_layer_state | 1u;
    uint32_t opaque     = 0;
    for (uint8_t i = 0; i < 32; i++) {
        if (((candidates >> i) & 1) && action_for_key(i, key).code != ACTION_TRANSPARENT) opaque |= (uint32_t)1 << i;
    }
    return sval_app_engine_lookup(&engine, layer_state, default_layer_state, opaque);
}

static void put32(uint8_t *p, uint32_t v) {
    for (uint8_t i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint8_t status_flags(void) {
    return (engine.rule >= 0 ? 1 : 0) | (engine.default_set ? 2 : 0) | (engine.default_yield ? 4 : 0) | (sval_app_rules_enabled(rules) ? 0 : 8);
}

// APP_FOREGROUND (0x40), data after [0xDF] [0x40]: [op] ... -> [op] [status] ...
// docs/protocol.md, "App rules".
void sval_app_foreground_command(uint8_t *d, uint8_t length) {
    if (length < 2) return;
    uint8_t op = d[0];
    if (!is_keyboard_master()) {
        d[1] = SVAL_APP_STATUS_NOT_USB;
        return;
    }
    engine_init_once();
    if (op == SVAL_APP_FG_SET) {
        if (length < 6) {
            d[1] = SVAL_APP_STATUS_SHORT;
            return;
        }
        uint32_t id       = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
        uint8_t  lifetime = d[5];
        sval_app_engine_foreground(&engine, id, lifetime, timer_read32());
        update();
        memset(d + 1, 0, length - 1);
        d[2] = (uint8_t)engine.rule;
        d[3] = status_flags();
        return;
    }
    if (op == SVAL_APP_FG_CLEAR) {
        sval_app_engine_clear(&engine);
        update();
        memset(d + 1, 0, length - 1);
        return;
    }
    if (op == SVAL_APP_FG_STATUS) {
        if (length < 31) {
            d[1] = SVAL_APP_STATUS_SHORT;
            return;
        }
        update();
        memset(d + 1, 0, length - 1);
        uint32_t left = sval_app_engine_ms_left(&engine, timer_read32());
        put32(d + 2, left ? engine.app_id : 0);
        d[6]  = (uint8_t)left;
        d[7]  = (uint8_t)(left >> 8);
        d[8]  = (uint8_t)engine.rule;
        d[9]  = engine.front;
        d[10] = (uint8_t)engine.on_mask;
        d[11] = (uint8_t)(engine.on_mask >> 8);
        d[12] = (uint8_t)engine.off_mask;
        d[13] = (uint8_t)(engine.off_mask >> 8);
        put32(d + 14, engine.default_set ? engine.user_default : default_layer_state);
        put32(d + 18, default_layer_state);
        put32(d + 22, layer_state);
        put32(d + 26, sval_app_rules_layer_view(layer_state));
        d[30] = status_flags();
        return;
    }
    memset(d + 1, 0, length - 1);
    d[1] = SVAL_APP_STATUS_BAD;
}

// APP_RULES (0x41), data after [0xDF] [0x41]: [op] ... -> [op] [status] ...
void sval_app_rules_command(uint8_t *d, uint8_t length) {
    if (length < 2) return;
    uint8_t op = d[0];
    if (!is_keyboard_master()) {
        d[1] = SVAL_APP_STATUS_NOT_USB;
        return;
    }
    engine_init_once();
    switch (op) {
        case SVAL_APP_RULES_INFO: {
            if (length < 14) {
                d[1] = SVAL_APP_STATUS_SHORT;
                return;
            }
            memset(d + 1, 0, length - 1);
            uint16_t used = sval_app_rules_used(rules);
            d[2]          = SVAL_APP_RULES_FORMAT;
            d[3]          = SVAL_APP_RULES_MAX;
            d[4]          = SVAL_APP_RULE_HEAD_BYTES;
            d[5]          = sval_app_rules_count(rules);
            d[6]          = (uint8_t)SVAL_APP_RULES_RECORD_BYTES;
            d[7]          = (uint8_t)(SVAL_APP_RULES_RECORD_BYTES >> 8);
            d[8]          = SVAL_APP_RULES_CHUNK;
            d[9]          = layer_count();
            d[10]         = (uint8_t)used;
            d[11]         = (uint8_t)(used >> 8);
            d[12]         = SVAL_APP_RULE_TEXT_MAX;
            d[13]         = rules[2] & SVAL_APP_RULES_OFF;
            return;
        }
        case SVAL_APP_RULES_READ:
        case SVAL_APP_RULES_WRITE: {
            if (length < 4) {
                d[1] = SVAL_APP_STATUS_SHORT;
                return;
            }
            uint16_t off = (uint16_t)(d[1] | (d[2] << 8));
            uint8_t  len = d[3];
            if (len > SVAL_APP_RULES_CHUNK || (uint32_t)off + len > SVAL_APP_RULES_RECORD_BYTES) {
                d[1] = SVAL_APP_STATUS_BAD;
                return;
            }
            if (op == SVAL_APP_RULES_WRITE) {
                if (length < 4 + len) {
                    d[1] = SVAL_APP_STATUS_SHORT;
                    return;
                }
                memcpy(staging + off, d + 4, len);
                d[1] = SVAL_APP_STATUS_OK;
                return;
            }
            if (length < 5 + len) {
                d[1] = SVAL_APP_STATUS_SHORT;
                return;
            }
            d[4] = len;
            d[3] = (uint8_t)(off >> 8);
            d[2] = (uint8_t)off;
            d[1] = SVAL_APP_STATUS_OK;
            memcpy(d + 5, rules + off, len);
            return;
        }
        case SVAL_APP_RULES_BEGIN:
            memcpy(staging, rules, sizeof(rules));
            d[1] = SVAL_APP_STATUS_OK;
            return;
        case SVAL_APP_RULES_COMMIT: {
            if (length < 3) {
                d[1] = SVAL_APP_STATUS_SHORT;
                return;
            }
            if (!sval_app_rules_valid(staging)) {
                d[1] = SVAL_APP_STATUS_INVALID;
                return;
            }
            sval_app_rules_tidy(staging);
            memcpy(rules, staging, sizeof(rules));
            bool saved = true;
#if VIA_ENABLE
            saved = settings_area_put(SVAL_SET_APP_RULES, rules, sizeof(rules));
#endif
            update();
            d[1] = saved ? SVAL_APP_STATUS_OK : SVAL_APP_STATUS_NOT_SAVED;
            d[2] = sval_app_rules_count(rules);
            return;
        }
        default:
            d[1] = SVAL_APP_STATUS_BAD;
            return;
    }
}

