// SPDX-License-Identifier: GPL-2.0-or-later
// Waking a suspended host (usb_wake.c). The wake check runs from housekeeping,
// after keyboard_task() has processed the press that wakes the host, so that
// key's release must reach the keymap too: a layer key or a mod-tap's
// modifier must not stay on after waking the host.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
extern "C" {
#include "../../keyboards/svalboard/usb_wake.h"
}
using testing::_;
using testing::InSequence;

class SvalUsbWake : public TestFixture {};

// As svalboard.c's handler, once per main-loop pass while the host sleeps.
static bool wake_pass(void) {
    return sval_usb_wake_key_down();
}

TEST_F(SvalUsbWake, NoKeyNoWake) {
    TestDriver driver;
    set_keymap({KeymapKey(0, 0, 0, KC_A)});
    EXPECT_NO_REPORT(driver);
    run_one_scan_loop();
    EXPECT_FALSE(wake_pass());
    VERIFY_AND_CLEAR(driver);
}

TEST_F(SvalUsbWake, LayerKeyReleasedAfterWake) {
    TestDriver driver;
    auto       mo = KeymapKey(0, 0, 0, MO(1));
    auto       a  = KeymapKey(0, 1, 0, KC_A);
    auto       b  = KeymapKey(1, 1, 0, KC_B);
    set_keymap({mo, a, b});

    EXPECT_NO_REPORT(driver);
    mo.press();
    run_one_scan_loop();
    EXPECT_TRUE(layer_state_is(1));
    EXPECT_TRUE(wake_pass());
    mo.release();
    run_one_scan_loop();
    EXPECT_FALSE(wake_pass());
    EXPECT_FALSE(layer_state_is(1));
    VERIFY_AND_CLEAR(driver);

    // The next key is on the base layer.
    {
        InSequence order;
        EXPECT_REPORT(driver, (KC_A));
        EXPECT_EMPTY_REPORT(driver);
    }
    tap_key(a);
    VERIFY_AND_CLEAR(driver);
}

TEST_F(SvalUsbWake, ModTapModifierReleasedAfterWake) {
    TestDriver driver;
    auto       mt = KeymapKey(0, 0, 0, LSFT_T(KC_A));
    set_keymap({mt});

    EXPECT_NO_REPORT(driver);
    mt.press();
    idle_for(TAPPING_TERM);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_LSFT));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);
    EXPECT_TRUE(wake_pass());

    EXPECT_EMPTY_REPORT(driver);
    mt.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EQ(get_mods(), 0);
}

TEST_F(SvalUsbWake, KeyReleasedAfterWake) {
    TestDriver driver;
    auto       a = KeymapKey(0, 0, 0, KC_A);
    set_keymap({a});

    EXPECT_REPORT(driver, (KC_A));
    a.press();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_TRUE(wake_pass());
    EXPECT_EMPTY_REPORT(driver);
    a.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}
