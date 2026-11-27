// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include "app_rules.h"

uint32_t sval_app_id(const char *key, uint16_t len) {
    while (len && (key[0] == ' ' || key[0] == '\t')) {
        key++;
        len--;
    }
    while (len && (key[len - 1] == ' ' || key[len - 1] == '\t')) len--;
    if (!len) return 0;
    uint32_t h = 0x811C9DC5u;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)key[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        h = (h ^ c) * 0x01000193u;
    }
    return h ? h : 1;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

// Bytes the rules of a format 2 header take, walking them; 0 when they do not fit.
static uint16_t walk(const uint8_t *rec) {
    uint16_t off = SVAL_APP_RULES_HEADER_BYTES;
    for (uint8_t i = 0; i < rec[1]; i++) {
        if (off + SVAL_APP_RULE_HEAD_BYTES > SVAL_APP_RULES_RECORD_BYTES) return 0;
        uint8_t n = rec[off + 11];
        if (n > SVAL_APP_RULE_TEXT_MAX || off + SVAL_APP_RULE_HEAD_BYTES + n > SVAL_APP_RULES_RECORD_BYTES) return 0;
        off = (uint16_t)(off + SVAL_APP_RULE_HEAD_BYTES + n);
    }
    return off;
}

static bool blank(const uint8_t *rec) {
    return rec[0] == 0 && rec[1] == 0;
}

bool sval_app_rules_valid(const uint8_t *rec) {
    if (blank(rec)) return true; // never written
    return rec[0] == SVAL_APP_RULES_FORMAT && rec[1] <= SVAL_APP_RULES_MAX && walk(rec) != 0;
}

uint8_t sval_app_rules_count(const uint8_t *rec) {
    return !blank(rec) && sval_app_rules_valid(rec) ? rec[1] : 0;
}

uint16_t sval_app_rules_used(const uint8_t *rec) {
    if (blank(rec)) return SVAL_APP_RULES_HEADER_BYTES;
    return sval_app_rules_valid(rec) ? walk(rec) : 0;
}

bool sval_app_rules_enabled(const uint8_t *rec) {
    return !(rec[2] & SVAL_APP_RULES_OFF);
}

void sval_app_rules_set_enabled(uint8_t *rec, bool on) {
    if (blank(rec)) rec[0] = SVAL_APP_RULES_FORMAT;
    rec[2] = on ? (uint8_t)(rec[2] & ~SVAL_APP_RULES_OFF) : (uint8_t)(rec[2] | SVAL_APP_RULES_OFF);
}

uint16_t sval_app_rule_offset(const uint8_t *rec, uint8_t index) {
    uint16_t off = SVAL_APP_RULES_HEADER_BYTES;
    for (uint8_t i = 0; i < index; i++) off = (uint16_t)(off + SVAL_APP_RULE_HEAD_BYTES + rec[off + 11]);
    return off;
}

void sval_app_rule_get(const uint8_t *rec, uint8_t index, sval_app_rule_t *rule) {
    const uint8_t *p    = rec + sval_app_rule_offset(rec, index);
    rule->app_id        = rd32(p);
    rule->default_layer = p[4];
    rule->front_layer   = p[5];
    rule->on_mask       = rd16(p + 6);
    rule->off_mask      = rd16(p + 8);
    rule->flags         = p[10];
}

const uint8_t *sval_app_rule_text(const uint8_t *rec, uint8_t index, uint8_t *len) {
    const uint8_t *p = rec + sval_app_rule_offset(rec, index);
    *len             = p[11];
    return p + SVAL_APP_RULE_HEAD_BYTES;
}

bool sval_app_rules_append(uint8_t *rec, const sval_app_rule_t *rule, const char *text) {
    if (blank(rec)) rec[0] = SVAL_APP_RULES_FORMAT;
    if (!sval_app_rules_valid(rec) || rec[1] >= SVAL_APP_RULES_MAX) return false;
    size_t   n   = text ? strlen(text) : 0;
    uint16_t off = walk(rec);
    if (n > SVAL_APP_RULE_TEXT_MAX) n = SVAL_APP_RULE_TEXT_MAX;
    if (off + SVAL_APP_RULE_HEAD_BYTES + n > SVAL_APP_RULES_RECORD_BYTES) return false;
    uint8_t *p = rec + off;
    wr16(p, (uint16_t)rule->app_id);
    wr16(p + 2, (uint16_t)(rule->app_id >> 16));
    p[4] = rule->default_layer;
    p[5] = rule->front_layer;
    wr16(p + 6, rule->on_mask);
    wr16(p + 8, rule->off_mask);
    p[10] = rule->flags;
    p[11] = (uint8_t)n;
    if (n) memcpy(p + SVAL_APP_RULE_HEAD_BYTES, text, n);
    rec[1]++;
    return true;
}

int8_t sval_app_rules_match(const uint8_t *rec, uint32_t app_id) {
    if (!app_id || !sval_app_rules_enabled(rec)) return -1;
    uint8_t  n   = sval_app_rules_count(rec);
    uint16_t off = SVAL_APP_RULES_HEADER_BYTES;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *p = rec + off;
        if (rd32(p) == app_id && !(p[10] & SVAL_APP_RULE_OFF)) return (int8_t)i;
        off = (uint16_t)(off + SVAL_APP_RULE_HEAD_BYTES + p[11]);
    }
    return -1;
}

void sval_app_rules_tidy(uint8_t *rec) {
    if (blank(rec) || !sval_app_rules_valid(rec)) {
        memset(rec, 0, SVAL_APP_RULES_RECORD_BYTES);
        return;
    }
    rec[2] &= SVAL_APP_RULES_OFF;
    rec[3]        = 0;
    uint16_t used = walk(rec);
    memset(rec + used, 0, SVAL_APP_RULES_RECORD_BYTES - used);
}

// Format 1: [1][count][0][0], then 24-byte rules: id u32, default, front, on
// u16, off u16, flags, 13-byte label.
#define V1_RULE_BYTES 24
#define V1_LABEL_BYTES 13

bool sval_app_rules_upgrade(const uint8_t *in, uint16_t len, uint8_t *out) {
    if (len < SVAL_APP_RULES_HEADER_BYTES || in[0] != 1 || in[1] > SVAL_APP_RULES_MAX) return false;
    memset(out, 0, SVAL_APP_RULES_RECORD_BYTES);
    out[0] = SVAL_APP_RULES_FORMAT;
    for (uint8_t i = 0; i < in[1]; i++) {
        uint16_t o = (uint16_t)(SVAL_APP_RULES_HEADER_BYTES + i * V1_RULE_BYTES);
        if (o + V1_RULE_BYTES > len) break;
        const uint8_t  *p = in + o;
        sval_app_rule_t r = {rd32(p), p[4], p[5], rd16(p + 6), rd16(p + 8), 0};
        char            label[V1_LABEL_BYTES + 1];
        memcpy(label, p + 11, V1_LABEL_BYTES);
        label[V1_LABEL_BYTES] = 0;
        // A site: its id is the hash of "site:" and the label, or the label is "…" and the site's end.
        char    key[5 + V1_LABEL_BYTES + 1] = "site:";
        uint8_t n                           = (uint8_t)strlen(label);
        memcpy(key + 5, label, (size_t)n + 1);
        bool cut = n > 3 && (uint8_t)label[0] == 0xE2 && (uint8_t)label[1] == 0x80 && (uint8_t)label[2] == 0xA6;
        if (n && (cut || sval_app_id(key, (uint16_t)(5 + n)) == r.app_id)) r.flags |= SVAL_APP_RULE_SITE;
        if (!sval_app_rules_append(out, &r, label)) break;
    }
    return true;
}

void sval_app_engine_init(sval_app_engine_t *e) {
    memset(e, 0, sizeof(*e));
    e->rule  = -1;
    e->front = SVAL_APP_LAYER_NONE;
}

void sval_app_engine_keep(sval_app_engine_t *e, uint16_t layers) {
    e->keep_mask = layers;
}

void sval_app_engine_foreground(sval_app_engine_t *e, uint32_t app_id, uint8_t lifetime_s, uint32_t now) {
    if (app_id != e->app_id) e->default_yield = false; // a new app: its rule may set the default layer again
    if (!lifetime_s) lifetime_s = SVAL_APP_LIFETIME_DEFAULT_S;
    if (lifetime_s > SVAL_APP_LIFETIME_MAX_S) lifetime_s = SVAL_APP_LIFETIME_MAX_S;
    e->app_id      = app_id;
    e->seen_at     = now;
    e->lifetime_ms = (uint32_t)lifetime_s * 1000u;
}

void sval_app_engine_clear(sval_app_engine_t *e) {
    e->app_id        = 0;
    e->lifetime_ms   = 0;
    e->default_yield = false;
}

uint32_t sval_app_engine_ms_left(const sval_app_engine_t *e, uint32_t now) {
    if (!e->app_id) return 0;
    uint32_t age = now - e->seen_at;
    return age < e->lifetime_ms ? e->lifetime_ms - age : 0;
}

bool sval_app_engine_update(sval_app_engine_t *e, const uint8_t *rec, uint8_t layer_count, uint32_t now, uint32_t current_default, uint32_t *set_default) {
    if (e->app_id && sval_app_engine_ms_left(e, now) == 0) sval_app_engine_clear(e); // the host went quiet
    e->rule = sval_app_rules_match(rec, e->app_id);
    sval_app_rule_t r;
    if (e->rule >= 0) {
        sval_app_rule_get(rec, (uint8_t)e->rule, &r);
    } else {
        memset(&r, 0, sizeof(r));
        r.default_layer = r.front_layer = SVAL_APP_LAYER_NONE;
    }
    uint16_t layers = (layer_count >= 16 ? 0xFFFF : (uint16_t)((1u << layer_count) - 1)) & (uint16_t)~e->keep_mask;
    e->on_mask      = r.on_mask & layers;
    e->off_mask     = r.off_mask & layers;
    e->front        = r.front_layer < layer_count && r.front_layer < 16 && !((e->off_mask >> r.front_layer) & 1) ? r.front_layer : SVAL_APP_LAYER_NONE;

    // The user changed the default layer while the rule held it: theirs stands.
    if (e->default_set && current_default != e->rule_default) {
        e->default_set   = false;
        e->default_yield = true;
        e->user_default  = current_default;
    }
    bool     want = e->rule >= 0 && !e->default_yield && r.default_layer < layer_count && r.default_layer < 32;
    uint32_t mask = want ? (uint32_t)1 << r.default_layer : 0;
    if (want) {
        if (!e->default_set) {
            e->user_default = current_default;
            e->default_set  = true;
        }
        e->rule_default = mask;
        if (current_default != mask) {
            *set_default = mask;
            return true;
        }
        return false;
    }
    if (e->default_set) {
        e->default_set = false;
        if (current_default != e->user_default) {
            *set_default = e->user_default;
            return true;
        }
    }
    return false;
}

uint32_t sval_app_engine_layers(const sval_app_engine_t *e, uint32_t user_state) {
    uint32_t layers = (user_state | e->on_mask) & ~(uint32_t)e->off_mask;
    if (e->front != SVAL_APP_LAYER_NONE) layers |= (uint32_t)1 << e->front;
    return layers;
}

bool sval_app_engine_shapes_lookup(const sval_app_engine_t *e) {
    return e->front != SVAL_APP_LAYER_NONE || e->on_mask || e->off_mask;
}

static int8_t highest(uint32_t m) {
    for (int8_t i = 31; i >= 0; i--)
        if ((m >> i) & 1) return i;
    return -1;
}

uint8_t sval_app_engine_lookup(const sval_app_engine_t *e, uint32_t user_state, uint32_t default_state, uint32_t opaque) {
    uint32_t masked = (user_state | e->on_mask) & ~(uint32_t)e->off_mask;
    if (e->front == SVAL_APP_LAYER_NONE) {
        int8_t l = highest((masked | default_state) & opaque);
        return l < 0 ? 0 : (uint8_t)l;
    }
    int8_t l = highest(masked & ~(default_state | 1u) & opaque);
    if (l >= 0) return (uint8_t)l;
    if ((opaque >> e->front) & 1) return e->front;
    l = highest(default_state & opaque);
    return l < 0 ? 0 : (uint8_t)l;
}
