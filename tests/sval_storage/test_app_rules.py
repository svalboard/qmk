"""Host tests for app rules (keyboards/svalboard/app_rules.c).

The rule engine as the board runs it: matching, each action, restore when the
app leaves or the host goes quiet, masks with off winning, held layer keys,
and Auto Mouse's view of the layers.

Run: python3 -m unittest discover -s tests/sval_storage -v
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KB = ROOT / 'keyboards/svalboard'

TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_rules.h"
#include "automouse_layers.h"
#define B(n) ((uint32_t)1 << (n))
#define NONE SVAL_APP_LAYER_NONE

static uint8_t rec[SVAL_APP_RULES_RECORD_BYTES];
static sval_app_engine_t e;
static uint32_t now, def = B(0); /* the board's default layer state */

/* Rules are added in order: i is the index the rule gets. */
static void rule(uint8_t i, const char *key, uint8_t dl, uint8_t fl, uint16_t on, uint16_t off) {
    sval_app_rule_t r = {sval_app_id(key, (uint16_t)strlen(key)), dl, fl, on, off, 0};
    assert(rec[1] == i);
    assert(sval_app_rules_append(rec, &r, key));
}
/* One board update: apply a default layer change as the board would. */
static void tick(void) {
    uint32_t set;
    if (sval_app_engine_update(&e, rec, 16, now, def, &set)) def = set;
}
static void front(const char *key) {
    sval_app_engine_foreground(&e, sval_app_id(key, (uint16_t)strlen(key)), 0, now);
    tick();
}
/* Lookup with every layer opaque except those in transparent. */
static uint8_t look(uint32_t user, uint32_t transparent) {
    return sval_app_engine_lookup(&e, user, def, ~transparent);
}

int main(void) {
    /* App IDs: the protocol's test values. */
    assert(sval_app_id("notepad.exe", 11) == 0xB884A32Cu);
    assert(sval_app_id("Notepad.EXE", 11) == 0xB884A32Cu);
    assert(sval_app_id(" notepad.exe\t", 13) == 0xB884A32Cu);
    assert(sval_app_id("chrome.exe", 10) == 0xFC0C788Du);
    assert(sval_app_id("com.apple.Safari", 16) == 0xECE537F6u);
    assert(sval_app_id("firefox", 7) == 0xF4D4D03Au);
    assert(sval_app_id("", 0) == 0 && sval_app_id("  ", 2) == 0);

    /* Records: never written is valid and empty; a bad header is not. */
    memset(rec, 0, sizeof(rec));
    assert(sval_app_rules_valid(rec) && sval_app_rules_count(rec) == 0);
    assert(sval_app_rules_enabled(rec)); /* never written: on */
    memset(rec, 0xFF, sizeof(rec));
    assert(!sval_app_rules_valid(rec));
    memset(rec, 0, sizeof(rec));
    rec[0] = 2; rec[1] = 65;
    assert(!sval_app_rules_valid(rec));
    rec[0] = 1; rec[1] = 0; /* format 1 is not used as is: it is upgraded */
    assert(!sval_app_rules_valid(rec));
    assert(SVAL_APP_RULES_RECORD_BYTES == 1792);

    /* Packed rules: each its 12 fixed bytes and its text. */
    memset(rec, 0, sizeof(rec));
    sval_app_rule_t w = {0x12345678u, 2, 3, 0x0102, 0x8001, SVAL_APP_RULE_SITE}, r;
    const char *longsite = "practice.svalboard.com";
    assert(sval_app_rules_append(rec, &w, longsite));
    assert(sval_app_rules_append(rec, &w, ""));
    assert(rec[0] == 2 && rec[1] == 2);
    assert(sval_app_rules_used(rec) == 4 + 12 + 22 + 12);
    sval_app_rule_get(rec, 0, &r);
    assert(r.app_id == w.app_id && r.default_layer == 2 && r.front_layer == 3 && r.on_mask == 0x0102 && r.off_mask == 0x8001 && r.flags == SVAL_APP_RULE_SITE);
    uint8_t tl;
    const uint8_t *tx = sval_app_rule_text(rec, 0, &tl);
    assert(tl == 22 && memcmp(tx, longsite, 22) == 0); /* the full text survives */
    assert(sval_app_rule_offset(rec, 1) == 4 + 12 + 22);
    sval_app_rule_get(rec, 1, &r); assert(r.app_id == w.app_id);
    /* Text past 64 bytes is cut; a text length past 64 is not valid. */
    char big[100]; memset(big, 'a', 99); big[99] = 0;
    assert(sval_app_rules_append(rec, &w, big));
    sval_app_rule_text(rec, 2, &tl); assert(tl == 64);
    rec[4 + 11] = 65; assert(!sval_app_rules_valid(rec)); rec[4 + 11] = 22;
    /* Tidy zeroes past the rules and keeps only the App Layers bit of byte 2. */
    uint16_t used = sval_app_rules_used(rec);
    rec[used + 5] = 0x55; rec[2] = 0xFE; rec[3] = 7;
    sval_app_rules_tidy(rec);
    assert(rec[used + 5] == 0 && rec[2] == 0 && rec[3] == 0 && sval_app_rules_enabled(rec));
    rec[2] = 0xFF; sval_app_rules_tidy(rec); assert(rec[2] == SVAL_APP_RULES_OFF);
    /* One shared budget: many short rules, or fewer long ones; never past the record. */
    memset(rec, 0, sizeof(rec));
    sval_app_rule_t s = {1, NONE, NONE, 0, 0, 0};
    for (int i = 0; i < 64; i++) { s.app_id = (uint32_t)i + 1; assert(sval_app_rules_append(rec, &s, "typical-app.exe")); } /* 64 rules of 15 characters fit */
    assert(!sval_app_rules_append(rec, &s, "x")); /* 64 rules at most */
    assert(sval_app_rules_used(rec) == 4 + 64 * (12 + 15));
    memset(rec, 0, sizeof(rec));
    int fit = 0;
    while (sval_app_rules_append(rec, &s, big)) fit++;
    assert(fit == (SVAL_APP_RULES_RECORD_BYTES - 4) / (12 + 64)); /* 23 rules of 64 bytes */
    assert(sval_app_rules_valid(rec) && sval_app_rules_used(rec) <= SVAL_APP_RULES_RECORD_BYTES);

    /* The App Layers switch: header byte 2 bit 0; the rules stay. Round trip. */
    memset(rec, 0, sizeof(rec));
    rule(0, "notepad.exe", 2, NONE, 0, 0);
    assert(rec[2] == 0 && sval_app_rules_enabled(rec)); /* a header without the bit: on */
    sval_app_rules_set_enabled(rec, false);
    assert(rec[2] == 1 && !sval_app_rules_enabled(rec) && sval_app_rules_count(rec) == 1);
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == -1);
    sval_app_rules_tidy(rec); assert(!sval_app_rules_enabled(rec) && sval_app_rules_count(rec) == 1);
    sval_app_rules_set_enabled(rec, true);
    assert(rec[2] == 0 && sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == 0);
    /* Off with no rules is kept too. */
    memset(rec, 0, sizeof(rec));
    sval_app_rules_set_enabled(rec, false); sval_app_rules_tidy(rec);
    assert(rec[0] == 2 && rec[1] == 0 && !sval_app_rules_enabled(rec) && sval_app_rules_used(rec) == 4);

    /* A rule is on unless its flags say off: zero flags (new, or from a file) are on. */
    memset(rec, 0, sizeof(rec));
    rule(0, "notepad.exe", 2, NONE, 0, 0);
    rule(1, "notepad.exe", 3, NONE, 0, 0);
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == 0);
    rec[4 + 10] = SVAL_APP_RULE_OFF; /* rule 0 off: the next one for the app applies */
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == 1);
    rec[4 + 10] = SVAL_APP_RULE_SITE; /* other flags leave it on */
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == 0);

    /* Format 1 (pre-release) upgrades: ids and layers kept, labels become text, App Layers on. */
    {
        static uint8_t v1[1540];
        memset(v1, 0, sizeof(v1));
        v1[0] = 1; v1[1] = 3;
        uint32_t ids[3] = {sval_app_id("notepad.exe", 11), sval_app_id("site:onshape.com", 16), 0xABCDEF01u};
        const char *labels[3] = {"notepad.exe", "onshape.com", "\xE2\x80\xA6lboard.com"};
        for (int i = 0; i < 3; i++) {
            uint8_t *q = v1 + 4 + i * 24;
            q[0] = (uint8_t)ids[i]; q[1] = (uint8_t)(ids[i] >> 8); q[2] = (uint8_t)(ids[i] >> 16); q[3] = (uint8_t)(ids[i] >> 24);
            q[4] = (uint8_t)(i + 1); q[5] = NONE; q[6] = 0x08; q[8] = 0x10;
            memcpy(q + 11, labels[i], strlen(labels[i]));
        }
        memset(rec, 0xAA, sizeof(rec));
        assert(sval_app_rules_upgrade(v1, sizeof(v1), rec));
        assert(sval_app_rules_valid(rec) && sval_app_rules_count(rec) == 3 && sval_app_rules_enabled(rec));
        for (uint8_t i = 0; i < 3; i++) {
            sval_app_rule_get(rec, i, &r);
            assert(r.app_id == ids[i] && r.default_layer == i + 1 && r.front_layer == NONE && r.on_mask == 0x08 && r.off_mask == 0x10);
            tx = sval_app_rule_text(rec, i, &tl);
            assert(tl == strlen(labels[i]) && memcmp(tx, labels[i], tl) == 0);
        }
        sval_app_rule_get(rec, 0, &r); assert(r.flags == 0);                  /* a program */
        sval_app_rule_get(rec, 1, &r); assert(r.flags == SVAL_APP_RULE_SITE); /* its id is the site's */
        sval_app_rule_get(rec, 2, &r); assert(r.flags == SVAL_APP_RULE_SITE); /* a site cut short */
        assert(sval_app_rules_match(rec, ids[0]) == 0);
        v1[0] = 2; assert(!sval_app_rules_upgrade(v1, sizeof(v1), rec));
    }

    /* Matching: first rule wins, empty rules and app 0 never match. */
    memset(rec, 0, sizeof(rec));
    rule(0, "chrome.exe", 1, NONE, 0, 0);
    rule(1, "notepad.exe", 2, NONE, 0, 0);
    rule(2, "notepad.exe", 3, NONE, 0, 0);
    { sval_app_rule_t z = {0, 1, NONE, 0, 0, 0}; assert(sval_app_rules_append(rec, &z, "")); } /* rule 3: app id 0 */
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == 1);
    assert(sval_app_rules_match(rec, 0) == -1);
    assert(sval_app_rules_match(rec, 0x12345u) == -1);
    rec[1] = 1; /* count limits matching */
    assert(sval_app_rules_match(rec, sval_app_id("notepad.exe", 11)) == -1);

    /* 1. Default layer: set while in front, user's back when it leaves. */
    memset(rec, 0, sizeof(rec));
    rule(0, "notepad.exe", 2, NONE, 0, 0);
    rule(1, "chrome.exe", 4, NONE, 0, 0);
    sval_app_engine_init(&e);
    def = B(1); /* the user's own default is layer 1 */
    tick(); assert(def == B(1) && e.rule == -1);
    front("notepad.exe"); assert(def == B(2) && e.rule == 0 && e.default_set);
    front("chrome.exe"); assert(def == B(4) && e.rule == 1);
    front("explorer.exe"); assert(def == B(1) && e.rule == -1 && !e.default_set);
    /* The lookup is QMK's: no masks or front layer. */
    assert(!sval_app_engine_shapes_lookup(&e));

    /* Restore on timeout: 15 s with no message. */
    front("notepad.exe"); assert(def == B(2));
    now += 14999; tick(); assert(def == B(2));
    assert(sval_app_engine_ms_left(&e, now) == 1);
    now += 1; tick(); assert(def == B(1) && e.app_id == 0 && e.rule == -1);
    /* Renewed messages keep it: every 5 s for a minute. */
    front("notepad.exe");
    for (int i = 0; i < 12; i++) { now += 5000; front("notepad.exe"); assert(def == B(2)); }
    /* A lifetime of its own, capped at 60 s. */
    sval_app_engine_foreground(&e, sval_app_id("notepad.exe", 11), 200, now);
    assert(e.lifetime_ms == 60000);
    sval_app_engine_foreground(&e, sval_app_id("notepad.exe", 11), 3, now);
    now += 3000; tick(); assert(def == B(1));
    /* CLEAR (the host leaving) restores at once. */
    front("notepad.exe"); assert(def == B(2));
    sval_app_engine_clear(&e); tick(); assert(def == B(1));

    /* The user changes the default layer under the rule: theirs stands. */
    front("notepad.exe"); assert(def == B(2));
    def = B(3); tick(); assert(def == B(3) && e.default_yield && !e.default_set);
    now += 5000; front("notepad.exe"); assert(def == B(3)); /* renewals leave it */
    front("explorer.exe"); assert(def == B(3)); /* nothing to restore: the user's is in place */
    front("notepad.exe"); assert(def == B(2)); /* a new focus: the rule applies again */
    front("explorer.exe"); assert(def == B(3)); /* and gives back the user's new default */
    def = B(1);

    /* App Layers switched off: the user's default layer and layers come back at once. */
    memset(rec, 0, sizeof(rec));
    rule(0, "notepad.exe", 2, 5, B(3), B(1));
    sval_app_engine_init(&e);
    def = B(1);
    front("notepad.exe"); assert(def == B(2) && e.front == 5 && sval_app_engine_shapes_lookup(&e));
    sval_app_rules_set_enabled(rec, false);
    tick(); /* the next update (COMMIT runs one at once) */
    assert(def == B(1) && e.rule == -1 && !sval_app_engine_shapes_lookup(&e));
    assert(sval_app_engine_layers(&e, B(1)) == B(1));
    now += 5000; front("notepad.exe"); assert(def == B(1) && e.rule == -1); /* stays off with the app in front */
    sval_app_rules_set_enabled(rec, true); tick();
    assert(def == B(2) && e.rule == 0); /* back on: the rule applies again */
    /* A rule switched off lets go the same way. */
    rec[4 + 10] = SVAL_APP_RULE_OFF; tick();
    assert(def == B(1) && e.rule == -1 && !sval_app_engine_shapes_lookup(&e));
    rec[4 + 10] = 0; tick(); assert(def == B(2));
    sval_app_engine_clear(&e); tick(); assert(def == B(1));

    /* A layer past the board's layer count is ignored. */
    memset(rec, 0, sizeof(rec));
    rule(0, "notepad.exe", 20, 20, 0, 0);
    sval_app_engine_init(&e);
    front("notepad.exe"); assert(def == B(1) && e.front == NONE && e.rule == 0);

    /* 2. Front layer: just above the default; the user's layers stay above it. */
    memset(rec, 0, sizeof(rec));
    rule(0, "code.exe", NONE, 5, 0, 0);
    sval_app_engine_init(&e);
    def = B(0);
    assert(look(0, 0) == 0); /* no rule: QMK's lookup */
    front("code.exe");
    assert(sval_app_engine_shapes_lookup(&e) && e.front == 5);
    assert(look(0, 0) == 5);
    assert(look(0, B(5)) == 0); /* transparent on the front layer: the default layer */
    assert(look(B(2), 0) == 2); /* a held layer below its number still wins */
    assert(look(B(2), B(2)) == 5); /* transparent there: the front layer */
    assert(look(B(7), 0) == 7);
    assert(sval_app_engine_layers(&e, B(2)) == (B(2) | B(5)));
    front("explorer.exe"); assert(!sval_app_engine_shapes_lookup(&e) && look(0, 0) == 0);

    /* 3. Masks: (user | on) & ~off; off wins over on and over the front layer. */
    memset(rec, 0, sizeof(rec));
    rule(0, "game.exe", NONE, NONE, B(3) | B(4), B(4) | B(6));
    rule(1, "both.exe", NONE, 6, 0, B(6));
    sval_app_engine_init(&e);
    front("game.exe");
    assert(sval_app_engine_layers(&e, 0) == B(3));
    assert(look(0, 0) == 3);
    assert(look(B(6), 0) == 3); /* the user's layer 6 is held but masked off */
    assert(look(B(8), 0) == 8); /* a held layer the mask leaves alone works */
    assert(look(B(8), B(8)) == 3);
    assert(look(0, B(3)) == 0);
    /* The default layer is never masked off. */
    def = B(4); assert(look(0, B(3)) == 4); def = B(0);
    front("both.exe"); assert(e.front == NONE && sval_app_engine_shapes_lookup(&e));
    assert(sval_app_engine_layers(&e, 0) == 0);
    front("nothing.exe"); assert(sval_app_engine_layers(&e, B(6)) == B(6)); /* restored: nothing written */

    /* Masks beyond the layer count are dropped. */
    memset(rec, 0, sizeof(rec));
    rule(0, "a.exe", NONE, NONE, 0xFFFF, 0);
    sval_app_engine_init(&e);
    sval_app_engine_foreground(&e, sval_app_id("a.exe", 5), 0, now);
    uint32_t set;
    sval_app_engine_update(&e, rec, 8, now, def, &set);
    assert(e.on_mask == 0xFF);

    /* All three actions together. */
    memset(rec, 0, sizeof(rec));
    rule(0, "all.exe", 1, 2, B(3), B(4));
    sval_app_engine_init(&e);
    def = B(0);
    front("all.exe");
    assert(def == B(1) && e.front == 2 && e.on_mask == B(3) && e.off_mask == B(4));
    assert(look(B(4), 0) == 3);
    assert(look(B(4), B(3)) == 2);
    assert(look(B(4), B(3) | B(2)) == 1);
    now += 15000; tick();
    assert(def == B(0) && !sval_app_engine_shapes_lookup(&e) && look(B(4), 0) == 4);

    /* Auto Mouse "Only on Selected Layers" sees the layers the rule leaves on. */
    memset(rec, 0, sizeof(rec));
    rule(0, "cad.exe", NONE, 2, 0, 0);
    sval_app_engine_init(&e);
    def = B(0);
    front("cad.exe");
    uint32_t view = sval_app_engine_layers(&e, 0);
    assert(sval_automouse_layer_blocked(1, B(2), view, def, 15));  /* off on layer 2: the front layer counts */
    assert(!sval_automouse_layer_blocked(1, B(2), 0, def, 15));    /* without the rule it would not */
    view = sval_app_engine_layers(&e, B(15)); /* the mouse layer on over it */
    assert(sval_automouse_current_layer(view, def, 15) == 2);
    /* The auto mouse layer is Auto Mouse's: no mask turns it on or off. */
    memset(rec, 0, sizeof(rec));
    rule(0, "cad.exe", NONE, NONE, B(15) | B(3), B(15) | B(1));
    sval_app_engine_init(&e);
    sval_app_engine_keep(&e, B(15));
    front("cad.exe");
    assert(e.on_mask == B(3) && e.off_mask == B(1));
    assert(look(B(15), 0) == 15); /* Auto Mouse turned it on: its keys work */
    assert(sval_app_engine_layers(&e, 0) == B(3)); /* the rule never turns it on */
    assert(sval_app_engine_layers(&e, B(15) | B(1)) == (B(15) | B(3)));
    puts("app rules: ok");
    return 0;
}
'''


class AppRules(unittest.TestCase):
    def test_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(TEST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover', '-I',
                            str(KB), str(path / 'test.c'),
                            str(KB / 'app_rules.c'),
                            str(KB / 'automouse_layers.c'), '-o',
                            str(path / 'test')],
                           check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_budget(self):
        out = subprocess.run(['python3', str(KB / 'tools/settings_budget.py')], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertNotIn('WARNING', out.stdout)


if __name__ == '__main__':
    unittest.main()
