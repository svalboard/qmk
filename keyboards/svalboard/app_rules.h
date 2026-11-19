// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// App rules: layer changes while a given app is in front on the computer.
// A host app (Keybard Host) tells the board which app is in front
// (APP_FOREGROUND, 0x40); the rules live on the board (APP_RULES, 0x41,
// settings area record 10). keyboards/svalboard/docs/protocol.md, "App rules".
//
// Nothing a rule does is stored, and the user's own layer state is never
// written:
// - default layer: the board's default layer becomes the rule's while the app
//   is in front (runtime only, never saved), and the user's own comes back
//   when it leaves. If the user changes the default layer meanwhile, theirs
//   stands and the rule leaves it alone until the app changes.
// - front layer: a layer just above the default layer; layers the user turns
//   on (held or toggled) stay above it.
// - on and off masks: applied over the user's layer state when keys are
//   looked up: (user | on) & ~off. Off wins over on. The default layer is not
//   masked.
// With no foreground message for the lifetime (15 s by default), or none
// since power-up, no rule applies.
//
// Pure functions: no QMK headers, so the host tests build them as they are.

#include <stdbool.h>
#include <stdint.h>

#define SVAL_APP_RULES_MAX 64
#define SVAL_APP_RULE_BYTES 24
#define SVAL_APP_RULES_HEADER_BYTES 4
#define SVAL_APP_RULES_RECORD_BYTES (SVAL_APP_RULES_HEADER_BYTES + SVAL_APP_RULES_MAX * SVAL_APP_RULE_BYTES) // 1540
#define SVAL_APP_RULES_FORMAT 1
#define SVAL_APP_RULE_LABEL_BYTES 13
#define SVAL_APP_LAYER_NONE 0xFF
#define SVAL_APP_LIFETIME_DEFAULT_S 15
#define SVAL_APP_LIFETIME_MAX_S 60

// Record: [format 1][count][0][0], then count rules of 24 bytes (little-endian):
//   0 app id u32 (0 = empty, never matches)   4 default layer (FF none)
//   5 front layer (FF none)   6 on mask u16   8 off mask u16   10 flags (0)
//   11 label, 13 bytes UTF-8, zero-padded
typedef struct {
    uint32_t app_id;
    uint8_t  default_layer;
    uint8_t  front_layer;
    uint16_t on_mask;
    uint16_t off_mask;
    uint8_t  flags;
} sval_app_rule_t;

// The app id of a key (exe name, bundle ID, app_id or WM_CLASS): FNV-1a 32 over
// its bytes with ASCII letters lowercased and surrounding spaces dropped; 0 is
// returned as 1. An empty key is 0 (no app).
uint32_t sval_app_id(const char *key, uint16_t len);

// A record with format 1 and at most 64 rules. A blank record (all zero) is
// valid and has no rules.
bool    sval_app_rules_valid(const uint8_t *rec);
uint8_t sval_app_rules_count(const uint8_t *rec);
void    sval_app_rule_get(const uint8_t *rec, uint8_t index, sval_app_rule_t *rule);
void    sval_app_rule_put(uint8_t *rec, uint8_t index, const sval_app_rule_t *rule, const char *label);
// The first rule for app_id, or -1. App id 0 never matches.
int8_t sval_app_rules_match(const uint8_t *rec, uint32_t app_id);
// Zero what lies past the count, so stored bytes depend only on the rules.
void sval_app_rules_tidy(uint8_t *rec);

typedef struct {
    uint32_t app_id;        // app in front, 0 for none
    uint32_t seen_at;       // ms of the last foreground message
    uint32_t lifetime_ms;   // how long it holds without another
    int8_t   rule;          // rule that applies, -1 for none
    bool     default_set;   // the board's default layer is the rule's
    bool     default_yield; // the user changed the default layer under the rule: leave it until the app changes
    uint32_t user_default;  // the user's default layer state, restored when the rule lets go
    uint32_t rule_default;  // the default layer state the rule set
    uint8_t  front;         // front layer applied, FF none
    uint16_t on_mask;
    uint16_t off_mask;
} sval_app_engine_t;

void sval_app_engine_init(sval_app_engine_t *e);
// A foreground message: app_id now in front, for lifetime_s (0: the default).
void sval_app_engine_foreground(sval_app_engine_t *e, uint32_t app_id, uint8_t lifetime_s, uint32_t now);
// The host is leaving: no app is in front.
void sval_app_engine_clear(sval_app_engine_t *e);
// Expire, match and work out the actions. current_default is the board's
// default layer state now. Returns true when the board must set its default
// layer state to *set_default (runtime only).
bool sval_app_engine_update(sval_app_engine_t *e, const uint8_t *rec, uint8_t layer_count, uint32_t now, uint32_t current_default, uint32_t *set_default);
// ms until the foreground app expires, 0 when none.
uint32_t sval_app_engine_ms_left(const sval_app_engine_t *e, uint32_t now);
// The layers on for key lookup apart from the default layer: (user | on) & ~off, and the front layer.
uint32_t sval_app_engine_layers(const sval_app_engine_t *e, uint32_t user_state);
// Whether a rule changes key lookup now (front layer or masks).
bool sval_app_engine_shapes_lookup(const sval_app_engine_t *e);
// The layer a key's action comes from, given which layers have a
// non-transparent action for it (bit n: layer n does).
// No front layer: the highest of (masked layers | default layers), as QMK.
// With one: the highest masked layer, then the front layer, then the highest
// default layer; 0 if none.
uint8_t sval_app_engine_lookup(const sval_app_engine_t *e, uint32_t user_state, uint32_t default_state, uint32_t opaque);
