// SPDX-License-Identifier: GPL-2.0-or-later
// The module's tap dances finish on release when no later tap can change the
// result, as the Vial firmware's did.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
// sval.h is C; C++ spells _Static_assert as static_assert.
#define _Static_assert static_assert
extern "C" {
#include "../../modules/svalboard/core/sval.h"
}
using testing::_;
using testing::InSequence;

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

// As sval.c: basic keycodes are registered, the rest run as actions.
static void keycode_event(uint16_t keycode, bool pressed) {
    if (keycode <= QK_MODS_MAX) {
        pressed ? register_code16(keycode) : unregister_code16(keycode);
        return;
    }
    keyrecord_t record   = {};
    record.event.key     = {.col = 0xFF, .row = 0xFF};
    record.event.type    = KEY_EVENT;
    record.event.pressed = pressed;
    record.event.time    = timer_read() | 1;
    process_action(&record, action_for_keycode(keycode));
}
extern "C" void sval_keycode_down(uint16_t keycode) {
    keycode_event(keycode, true);
}
extern "C" void sval_keycode_up(uint16_t keycode) {
    keycode_event(keycode, false);
}
extern "C" void sval_keycode_tap(uint16_t keycode) {
    sval_keycode_down(keycode);
    sval_keycode_up(keycode);
}

class SvalTapDance : public TestFixture {
   public:
    void dance(uint16_t tap, uint16_t hold, uint16_t double_tap, uint16_t tap_hold) {
        memset(dances, 0, sizeof(dances));
        dances[0] = {tap, hold, double_tap, tap_hold, 0x8000 | 370};
        sval_reload_tap_dance();
    }
};

// A fast double-tap and then another key: Double-tap fires on the second
// release, and the next key resolves on the layer it turned on.
TEST_F(SvalTapDance, DoubleTapFinishesOnSecondRelease) {
    TestDriver driver;
    auto       td  = KeymapKey(0, 0, 0, TD(0));
    auto       a   = KeymapKey(0, 1, 0, KC_A);
    auto       esc = KeymapKey(13, 1, 0, KC_ESC);
    set_keymap({td, a, esc});
    dance(KC_NO, KC_NO, TG(13), TG(13));

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(40);
    td.press();
    run_one_scan_loop();
    idle_for(30);
    td.release();
    run_one_scan_loop();
    EXPECT_TRUE(layer_state_is(13));
    VERIFY_AND_CLEAR(driver);

    idle_for(20); // well inside the 370 ms term
    {
        InSequence order;
        EXPECT_REPORT(driver, (KC_ESC));
        EXPECT_EMPTY_REPORT(driver);
    }
    tap_key(esc);
    VERIFY_AND_CLEAR(driver);

    // Nothing more happens when the term would have run out.
    EXPECT_NO_REPORT(driver);
    idle_for(400);
    EXPECT_TRUE(layer_state_is(13));
    VERIFY_AND_CLEAR(driver);
    layer_clear();
}

// Only Tap and Hold: the first release gives Tap at once.
TEST_F(SvalTapDance, TapAndHoldOnlyFinishesOnFirstRelease) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td});
    dance(KC_B, KC_C, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    td.press();
    run_one_scan_loop();
    idle_for(30);
    VERIFY_AND_CLEAR(driver);
    {
        InSequence order;
        EXPECT_REPORT(driver, (KC_B));
        EXPECT_EMPTY_REPORT(driver);
    }
    td.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);

    // Hold is still decided at the term.
    EXPECT_NO_REPORT(driver);
    td.press();
    idle_for(370);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_C));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    td.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

// A held second press waits for the term, so Tap-hold works as before.
TEST_F(SvalTapDance, HeldSecondPressStillWaitsForTerm) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td});
    dance(KC_B, KC_NO, KC_D, KC_C);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(40);
    td.press();
    idle_for(370);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_C));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    td.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

// Without Double-tap a dance can reach three taps, and nothing finishes early.
TEST_F(SvalTapDance, TripleTapPathIsUnchanged) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td});
    dance(KC_B, KC_NO, KC_NO, KC_C);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(40);
    tap_key(td, 30);
    idle_for(40);
    VERIFY_AND_CLEAR(driver);
    {
        InSequence order;
        for (int i = 0; i < 3; i++) {
            EXPECT_REPORT(driver, (KC_B));
            EXPECT_EMPTY_REPORT(driver);
        }
    }
    tap_key(td, 30);
    VERIFY_AND_CLEAR(driver);
    EXPECT_NO_REPORT(driver);
    idle_for(400);
    VERIFY_AND_CLEAR(driver);
}
