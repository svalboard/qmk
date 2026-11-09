// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_common.hpp"
extern "C" {
#include "action_tapping.h"
#include "process_grave_esc.h"
}
using testing::_;
using testing::InSequence;

static uint16_t     code_delay, caps_delay;
static uint8_t      toggle_count, overrides, auto_shift_bits;
extern "C" uint16_t get_tap_code_delay(void) {
    return code_delay;
}
extern "C" uint16_t get_tap_hold_caps_delay(void) {
    return caps_delay;
}
extern "C" uint8_t get_tapping_toggle(void) {
    return toggle_count ? toggle_count : 1;
}
extern "C" uint8_t get_grave_esc_overrides(void) {
    return overrides;
}
extern "C" uint8_t sval_auto_shift(void) {
    return auto_shift_bits;
}
extern "C" uint8_t get_oneshot_tap_toggle(void) {
    return 0;
}
extern "C" uint16_t get_oneshot_timeout(void) {
    return 0;
}

class RuntimeTap : public TestFixture {
   public:
    void SetUp() override {
        code_delay      = 0;
        caps_delay      = 80;
        toggle_count    = 5;
        overrides       = 0;
        auto_shift_bits = 0;
    }
    void expect_timed_tap(TestDriver &driver, uint8_t code, uint16_t delay, const std::function<void()> &act) {
        InSequence order;
        uint32_t   down = 0;
        EXPECT_CALL(driver, send_keyboard_mock(KeyboardReport(code))).WillOnce([&](report_keyboard_t &) { down = timer_read32(); });
        EXPECT_EMPTY_REPORT(driver).WillOnce([&](report_keyboard_t &) { EXPECT_EQ(timer_elapsed32(down), delay); });
        act();
        testing::Mock::VerifyAndClearExpectations(&driver);
    }
};

TEST_F(RuntimeTap, TapCodeAndTapCode16FollowChangesWithoutReboot) {
    TestDriver driver;
    for (uint16_t delay : {0, 37, 300}) {
        code_delay = delay;
        expect_timed_tap(driver, KC_A, delay, [] { tap_code(KC_A); });
        expect_timed_tap(driver, KC_B, delay, [] { tap_code16(KC_B); });
    }
}
TEST_F(RuntimeTap, CapsDelayIsIndependentOfOrdinaryTapDelay) {
    TestDriver driver;
    code_delay = 13;
    for (uint16_t delay : {0, 80, 300}) {
        caps_delay = delay;
        expect_timed_tap(driver, KC_CAPS, delay, [] { tap_code(KC_CAPS); });
        expect_timed_tap(driver, KC_CAPS, delay, [] { tap_code16(KC_CAPS); });
        expect_timed_tap(driver, KC_A, 13, [] { tap_code(KC_A); });
    }
}
TEST_F(RuntimeTap, SendStringAndSendCharPreserveFullRuntimeDelay) {
    TestDriver driver;
    for (uint16_t delay : {0, 37, 256, 300}) {
        code_delay = delay;
        expect_timed_tap(driver, KC_A, delay, [] { send_string("a"); });
        expect_timed_tap(driver, KC_B, delay, [] { send_char('b'); });
    }
}
TEST_F(RuntimeTap, ExplicitStringDelayStillOverridesRuntimeDefault) {
    TestDriver driver;
    code_delay = 300;
    expect_timed_tap(driver, KC_A, 0, [] { send_string_with_delay("a", 0); });
    expect_timed_tap(driver, KC_A, 21, [] { send_string_with_delay("a", 21); });
}
TEST_F(RuntimeTap, GraveEscapeOverrideBitsAreIndependentAndReleaseOriginalKey) {
    TestDriver    driver;
    const uint8_t mods[] = {MOD_LALT | MOD_LSFT, MOD_LCTL | MOD_LSFT, MOD_LGUI, MOD_LSFT};
    for (uint8_t bit = 0; bit < 4; bit++) {
        for (bool enabled : {false, true}) {
            InSequence order;
            overrides = enabled ? 1 << bit : 0;
            set_mods(mods[bit]);
            const uint8_t code = enabled ? KC_ESC : KC_GRV;
            EXPECT_CALL(driver, send_keyboard_mock(_)).WillOnce([&](report_keyboard_t &r) { EXPECT_EQ(r.keys[0], code); });
            EXPECT_CALL(driver, send_keyboard_mock(_)).WillOnce([&](report_keyboard_t &r) { EXPECT_EQ(r.keys[0], 0); });
            keyrecord_t record   = {};
            record.event.pressed = true;
            EXPECT_FALSE(process_grave_esc(QK_GRAVE_ESCAPE, &record));
            overrides ^= 15; // Editing the setting while held must not strand the key.
            clear_mods();
            record.event.pressed = false;
            EXPECT_FALSE(process_grave_esc(QK_GRAVE_ESCAPE, &record));
            testing::Mock::VerifyAndClearExpectations(&driver);
        }
    }
}
TEST_F(RuntimeTap, ToggleCountsOneThreeFiveAndZeroAlias) {
    TestDriver driver;
    auto       key         = KeymapKey(0, 0, 0, TT(1));
    auto       transparent = KeymapKey(1, 0, 0, KC_TRNS);
    set_keymap({key, transparent});
    EXPECT_NO_REPORT(driver);
    for (uint8_t count : {1, 3, 5, 0}) {
        toggle_count            = count;
        const uint8_t effective = count ? count : 1;
        layer_clear();
        idle_for(TAPPING_TERM + 1);
        for (uint8_t i = 1; i <= effective; i++) {
            tap_key(key, 10);
            EXPECT_EQ(layer_state, i == effective ? 2u : 0u);
            idle_for(10);
        }
        layer_clear();
        idle_for(TAPPING_TERM + 1);
        key.press();
        idle_for(TAPPING_TERM + 1);
        EXPECT_EQ(layer_state, 2u);
        key.release();
        run_one_scan_loop();
        EXPECT_EQ(layer_state, 0u);
    }
}
