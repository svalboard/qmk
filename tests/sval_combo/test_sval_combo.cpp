// SPDX-License-Identifier: GPL-2.0-or-later
// The module's combos are reloaded from storage whenever a host writes one. A
// combo held down at that moment still releases its output.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
// sval.h is C; C++ spells _Static_assert as static_assert.
#define _Static_assert static_assert
extern "C" {
#include "../../modules/svalboard/core/sval.h"
}
using testing::_;
using testing::InSequence;

static sval_combo_entry_t combos[SVAL_COMBO_ENTRIES];

extern "C" int sval_get_combo(uint16_t index, sval_combo_entry_t *entry) {
    if (index >= SVAL_COMBO_ENTRIES) return -1;
    *entry = combos[index];
    return 0;
}

// The runtime setting (sval_qmk_settings.c): the global combo term.
extern "C" uint16_t sval_get_combo_term(void) {
    return 50;
}

class SvalCombo : public TestFixture {
   public:
    // As Sval 0x04 (sval.c): store the entry, then reload every combo.
    void write_combo(uint8_t index, uint16_t k1, uint16_t k2, uint16_t output) {
        combos[index] = {{k1, k2, 0, 0}, output, 0x8000};
        sval_reload_combo();
    }
    void SetUp() override {
        memset(combos, 0, sizeof(combos));
        sval_reload_combo();
    }
};

// Hold a Shift combo, write another combo, let go: Shift comes up.
TEST_F(SvalCombo, HeldComboReleasesAfterAnotherComboIsWritten) {
    TestDriver driver;
    auto       a = KeymapKey(0, 0, 0, KC_A);
    auto       b = KeymapKey(0, 1, 0, KC_B);
    auto       c = KeymapKey(0, 2, 0, KC_C);
    set_keymap({a, b, c});
    write_combo(0, KC_A, KC_B, KC_LSFT);

    EXPECT_REPORT(driver, (KC_LSFT));
    a.press();
    run_one_scan_loop();
    b.press();
    run_one_scan_loop();
    idle_for(60);
    VERIFY_AND_CLEAR(driver);

    EXPECT_NO_REPORT(driver);
    write_combo(1, KC_B, KC_C, KC_ESC);
    VERIFY_AND_CLEAR(driver);

    EXPECT_EMPTY_REPORT(driver);
    a.release();
    run_one_scan_loop();
    b.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

// Rewrite the held combo itself: its old output comes up at once, and letting
// go sends nothing more.
TEST_F(SvalCombo, HeldComboReleasesWhenItIsRewritten) {
    TestDriver driver;
    auto       a = KeymapKey(0, 0, 0, KC_A);
    auto       b = KeymapKey(0, 1, 0, KC_B);
    set_keymap({a, b});
    write_combo(0, KC_A, KC_B, KC_LSFT);

    EXPECT_REPORT(driver, (KC_LSFT));
    a.press();
    run_one_scan_loop();
    b.press();
    run_one_scan_loop();
    idle_for(60);
    VERIFY_AND_CLEAR(driver);

    EXPECT_EMPTY_REPORT(driver);
    write_combo(0, KC_A, KC_B, KC_LCTL);
    VERIFY_AND_CLEAR(driver);

    EXPECT_NO_REPORT(driver);
    a.release();
    run_one_scan_loop();
    b.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);

    // The new output works.
    EXPECT_REPORT(driver, (KC_LCTL));
    a.press();
    run_one_scan_loop();
    b.press();
    run_one_scan_loop();
    idle_for(60);
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    a.release();
    run_one_scan_loop();
    b.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

// Disabling the held combo releases its output too.
TEST_F(SvalCombo, HeldComboReleasesWhenItIsDisabled) {
    TestDriver driver;
    auto       a = KeymapKey(0, 0, 0, KC_A);
    auto       b = KeymapKey(0, 1, 0, KC_B);
    set_keymap({a, b});
    write_combo(0, KC_A, KC_B, KC_LSFT);

    EXPECT_REPORT(driver, (KC_LSFT));
    a.press();
    run_one_scan_loop();
    b.press();
    run_one_scan_loop();
    idle_for(60);
    VERIFY_AND_CLEAR(driver);

    EXPECT_EMPTY_REPORT(driver);
    combos[0] = {};
    sval_reload_combo();
    VERIFY_AND_CLEAR(driver);

    // The keys' own releases may reach the keymap; nothing is held after.
    EXPECT_ANY_REPORT(driver).Times(testing::AnyNumber());
    a.release();
    run_one_scan_loop();
    b.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EQ(get_mods(), 0);
}
