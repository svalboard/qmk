// SPDX-License-Identifier: GPL-2.0-or-later
// Many tap dance keys can be held at once, as in the Vial firmware, where
// every dance kept its own state. QMK shares a pool of states, three by
// default, and ignores a press that finds the pool full.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
// sval.h is C; C++ spells _Static_assert as static_assert.
#define _Static_assert static_assert
extern "C" {
#include "../../modules/svalboard/core/sval.h"
}
using testing::_;
using testing::AnyNumber;

static sval_tap_dance_entry_t dances[SVAL_TAP_DANCE_ENTRIES];

extern "C" int sval_get_tap_dance(uint16_t index, sval_tap_dance_entry_t *entry) {
    if (index >= SVAL_TAP_DANCE_ENTRIES) return -1;
    *entry = dances[index];
    return 0;
}

// The runtime settings (sval_qmk_settings.c), at their defaults.
extern "C" uint16_t get_tap_code_delay(void) {
    return 0;
}
extern "C" uint16_t get_tap_hold_caps_delay(void) {
    return 80;
}
extern "C" uint8_t get_tapping_toggle(void) {
    return 5;
}
extern "C" uint8_t get_grave_esc_overrides(void) {
    return 0;
}
extern "C" uint8_t get_oneshot_tap_toggle(void) {
    return 0;
}
extern "C" uint16_t get_oneshot_timeout(void) {
    return 0;
}

// As sval_qmk_settings.c: a dance's own term.
extern "C" uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    if (keycode >= QK_TAP_DANCE && keycode <= QK_TAP_DANCE_MAX) {
        sval_tap_dance_entry_t td;
        if (sval_get_tap_dance(keycode & 0xFF, &td) == 0 && TD_ENABLED(td) && (td.custom_tapping_term & 0x7FFF)) return td.custom_tapping_term & 0x7FFF;
    }
    return TAPPING_TERM;
}

// As sval.c: basic keycodes are registered.
extern "C" void sval_keycode_down(uint16_t keycode) {
    register_code16(keycode);
}
extern "C" void sval_keycode_up(uint16_t keycode) {
    unregister_code16(keycode);
}
extern "C" void sval_keycode_tap(uint16_t keycode) {
    sval_keycode_down(keycode);
    sval_keycode_up(keycode);
}

class SvalTapDanceLimit : public TestFixture {};

// Eight dances, each holding one modifier, all held at once: every hold
// registers, and every release clears its own.
TEST_F(SvalTapDanceLimit, EightDancesHeldAtOnce) {
    TestDriver driver;
    EXPECT_CALL(driver, send_keyboard_mock(_)).Times(AnyNumber());
    static const uint16_t  holds[8] = {KC_LSFT, KC_LCTL, KC_LALT, KC_LGUI, KC_RSFT, KC_RCTL, KC_RALT, KC_RGUI};
    std::vector<KeymapKey> keys;
    for (uint8_t i = 0; i < 8; i++) {
        dances[i] = {(uint16_t)(KC_A + i), holds[i], KC_NO, KC_NO, 0x8000 | 200};
        keys.push_back(KeymapKey(0, i, 0, TD(i)));
    }
    set_keymap({keys[0], keys[1], keys[2], keys[3], keys[4], keys[5], keys[6], keys[7]});
    sval_reload_tap_dance();

    uint8_t expected = 0;
    for (uint8_t i = 0; i < 8; i++) {
        keys[i].press();
        idle_for(250); // past the term: a hold
        expected |= MOD_BIT(holds[i]);
        EXPECT_EQ(get_mods(), expected) << "dance " << (int)i << " held";
    }
    for (uint8_t i = 0; i < 8; i++) {
        keys[i].release();
        run_one_scan_loop();
        expected &= ~MOD_BIT(holds[i]);
        EXPECT_EQ(get_mods(), expected) << "dance " << (int)i << " released";
    }
    testing::Mock::VerifyAndClearExpectations(&driver);
}
