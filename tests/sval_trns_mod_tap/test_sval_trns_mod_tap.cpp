// SPDX-License-Identifier: GPL-2.0-or-later
// A mod-tap with KC_TRNS as its tap (trns_mod_tap.c) taps the key under it,
// also while a higher layer is on: the mouse layer (the highest) while Auto
// Mouse has it on.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
extern "C" {
#include "../../keyboards/svalboard/trns_mod_tap.h"
}
using testing::_;
using testing::InSequence;

#define MOUSE_LAYER 15 // as MH_AUTO_BUTTONS_LAYER: the highest layer

// As process_record_kb() in keymaps/keymap_support.c.
extern "C" bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    int8_t layer = sval_trns_mod_tap_layer(keycode, record);
    if (layer >= 0) {
        sval_trns_mod_tap_begin((uint8_t)layer, record->event.key);
        return true;
    }
    sval_trns_mod_tap_end(record);
    return true;
}

class SvalTrnsModTap : public TestFixture {
   public:
    KeymapKey a     = KeymapKey(0, 0, 0, KC_A);
    KeymapKey mt    = KeymapKey(1, 0, 0, LSFT_T(KC_TRNS));
    KeymapKey mouse = KeymapKey(MOUSE_LAYER, 0, 0, KC_TRNS);
    void      setup() {
        set_keymap({a, mt, mouse});
        layer_clear();
        layer_on(1);
    }
    void expect_tap_types_a(TestDriver &driver) {
        {
            InSequence order;
            EXPECT_REPORT(driver, (KC_A));
            EXPECT_EMPTY_REPORT(driver);
        }
        mt.press();
        run_one_scan_loop();
        idle_for(20);
        mt.release();
        run_one_scan_loop();
        idle_for(20);
        VERIFY_AND_CLEAR(driver);
    }
};

TEST_F(SvalTrnsModTap, TapTypesKeyBelow) {
    TestDriver driver;
    setup();
    expect_tap_types_a(driver);
    EXPECT_TRUE(layer_state_is(1));
}

// The mouse layer on, transparent here: the tap still types the key below the
// mod-tap's layer, and both layers stay as they were.
TEST_F(SvalTrnsModTap, TapTypesKeyBelowUnderMouseLayer) {
    TestDriver driver;
    setup();
    layer_on(MOUSE_LAYER);
    expect_tap_types_a(driver);
    EXPECT_TRUE(layer_state_is(MOUSE_LAYER));
    EXPECT_TRUE(IS_LAYER_ON(1));
    layer_clear();
}

TEST_F(SvalTrnsModTap, HoldIsTheModifier) {
    TestDriver driver;
    setup();
    layer_on(MOUSE_LAYER);
    EXPECT_NO_REPORT(driver);
    mt.press();
    idle_for(TAPPING_TERM);
    VERIFY_AND_CLEAR(driver);
    EXPECT_REPORT(driver, (KC_LSFT));
    idle_for(10);
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    mt.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_TRUE(IS_LAYER_ON(1));
    EXPECT_TRUE(IS_LAYER_ON(MOUSE_LAYER));
    layer_clear();
}
