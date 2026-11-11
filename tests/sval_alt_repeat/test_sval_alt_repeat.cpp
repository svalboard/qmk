// SPDX-License-Identifier: GPL-2.0-or-later
// The module's Alt Repeat entries match as the Vial firmware's did
// (quantum/vial.c in svalboard/vial-qmk), for every option bit.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
// sval.h is C; C++ spells _Static_assert as static_assert.
#define _Static_assert static_assert
extern "C" {
#include "../../modules/svalboard/core/sval.h"
}
using testing::_;
using testing::InSequence;

static sval_alt_repeat_key_entry_t entries[SVAL_ALT_REPEAT_KEY_ENTRIES];

extern "C" int sval_get_alt_repeat_key(uint16_t index, sval_alt_repeat_key_entry_t *entry) {
    if (index >= SVAL_ALT_REPEAT_KEY_ENTRIES) return -1;
    *entry = entries[index];
    return 0;
}

static const uint8_t EN    = sval_ark_enabled;
static const uint8_t DEF   = sval_ark_option_default_to_alt;
static const uint8_t BIDI  = sval_ark_option_bidirectional;
static const uint8_t HANDS = sval_ark_option_ignore_mod_handedness;

class SvalAltRepeat : public TestFixture {
   public:
    void SetUp() override {
        memset(entries, 0, sizeof(entries));
        sval_reload_alt_repeat_key();
    }
    void set(size_t i, uint16_t keycode, uint16_t alt, uint8_t allowed, uint8_t options) {
        entries[i] = {keycode, alt, allowed, options};
        sval_reload_alt_repeat_key();
    }
    uint16_t alt(uint16_t keycode, uint8_t mods = 0) {
        return get_alt_repeat_key_keycode_user(keycode, mods);
    }
};

// Enabled: only entries with the enabled bit match.
TEST_F(SvalAltRepeat, Enabled) {
    set(0, KC_A, KC_O, 0, 0);
    EXPECT_EQ(alt(KC_A), KC_TRNS);
    set(0, KC_A, KC_O, 0, EN);
    EXPECT_EQ(alt(KC_A), KC_O);
}

// Allowed mods may be held but need not be; any other held mod blocks the entry.
TEST_F(SvalAltRepeat, AllowedModsAreOptional) {
    set(0, KC_A, KC_O, MOD_BIT(KC_LSFT), EN);
    EXPECT_EQ(alt(KC_A), KC_O);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LSFT)), KC_O);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LCTL)), KC_TRNS);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_RSFT)), KC_TRNS);
}

// An entry with no allowed mods matches only with no mods held.
TEST_F(SvalAltRepeat, NoAllowedModsMeansNoMods) {
    set(0, KC_A, KC_O, 0, EN);
    EXPECT_EQ(alt(KC_A), KC_O);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LSFT)), KC_TRNS);
}

// A modded keycode's mods are required, and the entry with the most required
// mods wins whatever its position.
TEST_F(SvalAltRepeat, MostRequiredModsWins) {
    set(0, KC_A, KC_O, MOD_BIT(KC_LSFT), EN);
    set(1, LSFT(KC_A), KC_E, 0, EN);
    EXPECT_EQ(alt(KC_A), KC_O);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LSFT)), KC_E);
}

// The last key is reduced to its tap keycode: a mod-tap or layer-tap A
// matches an entry for A, and a modded keycode carries its mods.
TEST_F(SvalAltRepeat, TapKeysNormalize) {
    set(0, KC_A, KC_O, 0, EN);
    EXPECT_EQ(alt(LSFT_T(KC_A)), KC_O);
    EXPECT_EQ(alt(LT(1, KC_A)), KC_O);
    set(1, LCTL(KC_B), KC_F, 0, EN);
    EXPECT_EQ(alt(LCTL(KC_B)), KC_F);
    EXPECT_EQ(alt(KC_B, MOD_BIT(KC_LCTL)), KC_F);
    EXPECT_EQ(alt(KC_B), KC_TRNS);
}

// The alt keycode keeps its own mods.
TEST_F(SvalAltRepeat, AltKeepsMods) {
    set(0, KC_A, LSFT(KC_O), 0, EN);
    EXPECT_EQ(alt(KC_A), LSFT(KC_O));
    set(1, KC_B, RCTL(KC_F), 0, EN);
    EXPECT_EQ(alt(KC_B), RCTL(KC_F));
}

// Default to this alt key: a fallback for any last key with allowed mods,
// used only when no entry matches.
TEST_F(SvalAltRepeat, DefaultToThisAltKey) {
    set(0, KC_A, KC_O, 0, EN | DEF);
    EXPECT_EQ(alt(KC_A), KC_O);
    EXPECT_EQ(alt(KC_Z), KC_O);
    EXPECT_EQ(alt(KC_Z, MOD_BIT(KC_LSFT)), KC_TRNS);
    set(1, KC_Z, KC_X, 0, EN);
    EXPECT_EQ(alt(KC_Z), KC_X);
    EXPECT_EQ(alt(KC_Q), KC_O);
}

// Bidirectional: the alt key gives back the key, with mods checked.
TEST_F(SvalAltRepeat, Bidirectional) {
    set(0, KC_A, KC_O, 0, EN);
    EXPECT_EQ(alt(KC_O), KC_TRNS);
    set(0, KC_A, KC_O, 0, EN | BIDI);
    EXPECT_EQ(alt(KC_O), KC_A);
    EXPECT_EQ(alt(KC_O, MOD_BIT(KC_LCTL)), KC_TRNS);
    set(0, LSFT(KC_A), KC_O, 0, EN | BIDI);
    EXPECT_EQ(alt(KC_O), LSFT(KC_A));
}

// Ignore mod handedness: right-hand mods match left-hand mods and the reverse.
TEST_F(SvalAltRepeat, IgnoreModHandedness) {
    set(0, LSFT(KC_A), KC_O, MOD_BIT(KC_LCTL), EN);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_RSFT)), KC_TRNS);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LSFT) | MOD_BIT(KC_RCTL)), KC_TRNS);
    set(0, LSFT(KC_A), KC_O, MOD_BIT(KC_LCTL), EN | HANDS);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_RSFT)), KC_O);
    EXPECT_EQ(alt(KC_A, MOD_BIT(KC_LSFT) | MOD_BIT(KC_RCTL)), KC_O);
    EXPECT_EQ(alt(KC_A), KC_TRNS);
}

// End to end: Alt Repeat after a tapped mod-tap A types o.
TEST_F(SvalAltRepeat, AfterModTapKey) {
    TestDriver driver;
    KeymapKey  mt_a(0, 0, 0, LSFT_T(KC_A));
    KeymapKey  arep(0, 1, 0, QK_AREP);
    set_keymap({mt_a, arep});
    set(0, KC_A, KC_O, MOD_BIT(KC_LSFT), EN);

    EXPECT_REPORT(driver, (KC_A));
    EXPECT_EMPTY_REPORT(driver);
    tap_key(mt_a);
    testing::Mock::VerifyAndClearExpectations(&driver);
    EXPECT_REPORT(driver, (KC_O));
    EXPECT_EMPTY_REPORT(driver);
    tap_key(arep);
    testing::Mock::VerifyAndClearExpectations(&driver);
}
