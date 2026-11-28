// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_common.hpp"
#include <type_traits>
using testing::_;
extern "C" {
#include "action_tapping.h"
#include "process_grave_esc.h"
}
#ifdef RUNTIME_QMK_SETTINGS
#    error Static fixture must not use the runtime getters
#endif
static_assert(std::is_same<send_string_interval_t, uint8_t>::value, "Preserve the non-Sval send-string API");
class StaticTapSettings : public TestFixture {};
TEST_F(StaticTapSettings, CompileTimeGettersAndSendString) {
    TestDriver driver;
    EXPECT_EQ(GET_TAPPING_TOGGLE(), 3);
    EXPECT_EQ(GET_TAP_CODE_DELAY(), 11);
    EXPECT_EQ(GET_TAP_HOLD_CAPS_DELAY(), 83);
    uint32_t            down = 0;
    testing::InSequence order;
    EXPECT_REPORT(driver, (KC_A)).WillOnce([&](report_keyboard_t &) { down = timer_read32(); });
    EXPECT_EMPTY_REPORT(driver).WillOnce([&](report_keyboard_t &) { EXPECT_EQ(timer_elapsed32(down), 11u); });
    send_string("a");
}
TEST_F(StaticTapSettings, CompileTimeGraveEscapeOverrides) {
    TestDriver    driver;
    const uint8_t modifiers[] = {MOD_LALT | MOD_LSFT, MOD_LCTL | MOD_LSFT, MOD_LGUI, MOD_LSFT};
    for (uint8_t mods : modifiers) {
        testing::InSequence order;
        set_mods(mods);
        EXPECT_CALL(driver, send_keyboard_mock(testing::_)).WillOnce([](report_keyboard_t &r) { EXPECT_EQ(r.keys[0], KC_ESC); });
        EXPECT_CALL(driver, send_keyboard_mock(testing::_)).WillOnce([](report_keyboard_t &r) { EXPECT_EQ(r.keys[0], 0); });
        keyrecord_t record   = {};
        record.event.pressed = true;
        process_grave_esc(QK_GRAVE_ESCAPE, &record);
        clear_mods();
        record.event.pressed = false;
        process_grave_esc(QK_GRAVE_ESCAPE, &record);
        testing::Mock::VerifyAndClearExpectations(&driver);
    }
}
TEST_F(StaticTapSettings, ThreeTapsStillToggleLayer) {
    TestDriver driver;
    auto       key = KeymapKey(0, 0, 0, TT(1));
    set_keymap({key, KeymapKey(1, 0, 0, KC_TRNS)});
    EXPECT_NO_REPORT(driver);
    for (int n = 1; n <= 3; n++) {
        tap_key(key, 10);
        EXPECT_EQ(layer_state, n == 3 ? 2u : 0u);
    }
    layer_clear();
}
