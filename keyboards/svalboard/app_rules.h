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
#define SVAL_APP_RULES_HEADER_BYTES 4
#define SVAL_APP_RULE_HEAD_BYTES 12 // a rule's fixed fields, before its text
#define SVAL_APP_RULE_TEXT_MAX 64
#define SVAL_APP_RULES_RECORD_BYTES 1792 // the whole record: header, rules and their text
#define SVAL_APP_RULES_FORMAT 2
#define SVAL_APP_LAYER_NONE 0xFF
#define SVAL_APP_LIFETIME_DEFAULT_S 15
#define SVAL_APP_LIFETIME_MAX_S 60
// Header byte 2.
#define SVAL_APP_RULES_OFF 0x01 // App Layers off: no rule applies; the rules stay stored
// A rule's flags.
#define SVAL_APP_RULE_OFF 0x01  // this rule is off: matching skips it
#define SVAL_APP_RULE_SITE 0x02 // a website rule: its text is the site (for hosts; the board ignores it)

// Record (format 2): [format 2][count][flags][0], then count rules packed back
// to back, each its fixed fields and its text (little-endian):
//   0 app id u32 (0 = empty, never matches)   4 default layer (FF none)
//   5 front layer (FF none)   6 on mask u16   8 off mask u16   10 flags
//   11 text length n (at most 64)   12 text, n bytes UTF-8 (the app key, or the site)
// Header and rules together fit in SVAL_APP_RULES_RECORD_BYTES.
// Format 1 (never released: 24-byte rules with a 13-byte label) is upgraded
// when read (sval_app_rules_upgrade).
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

// A format 2 record whose rules fit. A blank record (all zero) is valid and
// has no rules.
bool    sval_app_rules_valid(const uint8_t *rec);
uint8_t sval_app_rules_count(const uint8_t *rec);
// Whether App Layers is on (header byte 2 bit 0 clear).
bool sval_app_rules_enabled(const uint8_t *rec);
void sval_app_rules_set_enabled(uint8_t *rec, bool on);
// Bytes the header and the rules use; 0 for a record that is not valid.
uint16_t sval_app_rules_used(const uint8_t *rec);
// Offset of rule index (index < count of a valid record).
uint16_t sval_app_rule_offset(const uint8_t *rec, uint8_t index);
void     sval_app_rule_get(const uint8_t *rec, uint8_t index, sval_app_rule_t *rule);
// A rule's text and its length.
const uint8_t *sval_app_rule_text(const uint8_t *rec, uint8_t index, uint8_t *len);
// Add a rule at the end (a blank record becomes format 2). False when it does
// not fit (64 rules, or the record's bytes); text longer than 64 bytes is cut.
bool sval_app_rules_append(uint8_t *rec, const sval_app_rule_t *rule, const char *text);
// The first rule for app_id that is on, or -1. App id 0 never matches, and
// nothing matches while App Layers is off.
int8_t sval_app_rules_match(const uint8_t *rec, uint32_t app_id);
// Zero what lies past the rules and the header's spare bits, so stored bytes
// depend only on the rules. A record that is not valid becomes blank.
void sval_app_rules_tidy(uint8_t *rec);
// A format 1 record (len bytes) as format 2 into out (the record's size), App
// Layers on. A label that names a site (its id is the hash of "site:" and it,
// or it starts with "…") marks a website rule. False if in is not format 1.
bool sval_app_rules_upgrade(const uint8_t *in, uint16_t len, uint8_t *out);

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
    uint16_t keep_mask; // layers no mask touches (the auto mouse layer)
} sval_app_engine_t;

void sval_app_engine_init(sval_app_engine_t *e);
// Layers the masks never turn on or off: the auto mouse layer, which Auto
// Mouse alone turns on and off (and the user's mouse keys).
void sval_app_engine_keep(sval_app_engine_t *e, uint16_t layers);
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
