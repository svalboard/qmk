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

// As sval.c: basic keycodes are registered, the rest go through action_exec()
// as a key event at a position of their own (sval.c's magic position, whose
// keycode it overrides), so they pass through process_record_quantum() and
// preprocess_tap_dance() as there. Here that position is OUT on layer 0, and a
// test that sends a keycode above QK_MODS_MAX puts that keycode there.
static const keypos_t OUT = {.col = 9, .row = 3};
static void           keycode_event(uint16_t keycode, bool pressed) {
    if (keycode <= QK_MODS_MAX) {
        pressed ? register_code16(keycode) : unregister_code16(keycode);
        return;
    }
    EXPECT_EQ(keymap_key_to_keycode(0, OUT), keycode);
    keyevent_t event = {};
    event.key        = OUT;
    event.type       = KEY_EVENT;
    event.pressed    = pressed;
    event.time       = timer_read() | 1;
    action_exec(event);
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
    set_keymap({td, a, esc, KeymapKey(0, OUT.col, OUT.row, TG(13)), KeymapKey(13, OUT.col, OUT.row, KC_TRNS)});
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

// Outputs above QK_MODS_MAX run through action_exec(), and so through
// preprocess_tap_dance() while the dance is still active. Each must still be
// released after it is pressed: MO(1) on Tap leaves layer 1 off.

// Tap decided at the term.
TEST_F(SvalTapDance, TapMomentaryLayerReleasedAtTerm) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td, KeymapKey(0, OUT.col, OUT.row, MO(1))});
    dance(MO(1), KC_NO, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(400);
    EXPECT_FALSE(layer_state_is(1));
    VERIFY_AND_CLEAR(driver);
}

// Tap decided on the release (a dance with only Tap and Hold).
TEST_F(SvalTapDance, TapMomentaryLayerReleasedOnRelease) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td, KeymapKey(0, OUT.col, OUT.row, MO(1))});
    dance(MO(1), KC_C, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    EXPECT_FALSE(layer_state_is(1));
    idle_for(400);
    EXPECT_FALSE(layer_state_is(1));
    VERIFY_AND_CLEAR(driver);
}

// Double-tap decided on the second release.
TEST_F(SvalTapDance, DoubleTapMomentaryLayerReleased) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td, KeymapKey(0, OUT.col, OUT.row, MO(2))});
    dance(KC_B, KC_NO, MO(2), KC_NO);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(40);
    tap_key(td, 30);
    EXPECT_FALSE(layer_state_is(2));
    idle_for(400);
    EXPECT_FALSE(layer_state_is(2));
    VERIFY_AND_CLEAR(driver);
}

// Tap decided by another key: that key resolves with the layer off again.
TEST_F(SvalTapDance, TapMomentaryLayerReleasedBeforeInterruptingKey) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    auto       a  = KeymapKey(0, 1, 0, KC_A);
    auto       x  = KeymapKey(1, 1, 0, KC_X);
    set_keymap({td, a, x, KeymapKey(0, OUT.col, OUT.row, MO(1))});
    dance(MO(1), KC_NO, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    tap_key(td, 30);
    idle_for(20);
    VERIFY_AND_CLEAR(driver);
    {
        InSequence order;
        EXPECT_REPORT(driver, (KC_A));
        EXPECT_EMPTY_REPORT(driver);
    }
    tap_key(a);
    EXPECT_FALSE(layer_state_is(1));
    VERIFY_AND_CLEAR(driver);
}

// A host rewrites a dance while its Hold is held: letting go releases the key
// the dance pressed, not the one stored now.
TEST_F(SvalTapDance, HeldDanceReleasesWhatItPressedAfterARewrite) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td});
    dance(KC_B, KC_LSFT, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    td.press();
    idle_for(370);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_LSFT));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);

    EXPECT_NO_REPORT(driver);
    dances[0].on_hold = KC_LCTL;
    sval_reload_tap_dance(); // as Sval 0x02 (sval.c) does after storing it
    VERIFY_AND_CLEAR(driver);

    EXPECT_EMPTY_REPORT(driver);
    td.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EQ(get_mods(), 0);
}

// The same when the host disables the dance while it is held.
TEST_F(SvalTapDance, HeldDanceReleasesWhatItPressedAfterItIsDisabled) {
    TestDriver driver;
    auto       td = KeymapKey(0, 0, 0, TD(0));
    set_keymap({td});
    dance(KC_B, KC_LSFT, KC_NO, KC_NO);

    EXPECT_NO_REPORT(driver);
    td.press();
    idle_for(370);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_LSFT));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);

    EXPECT_NO_REPORT(driver);
    dances[0].custom_tapping_term &= 0x7FFF; // enabled bit off
    sval_reload_tap_dance();
    VERIFY_AND_CLEAR(driver);

    EXPECT_EMPTY_REPORT(driver);
    td.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EQ(get_mods(), 0);
}
