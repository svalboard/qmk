// SPDX-License-Identifier: GPL-2.0-or-later
// The module's key overrides are reloaded from storage whenever a host writes
// one. An override active at that moment releases the key it sent.
#include "keyboard_report_util.hpp"
#include "test_common.hpp"
// sval.h is C; C++ spells _Static_assert as static_assert.
#define _Static_assert static_assert
extern "C" {
#include "../../modules/svalboard/core/sval.h"
}
using testing::_;
using testing::AnyNumber;

static sval_key_override_entry_t overrides[SVAL_KEY_OVERRIDE_ENTRIES];

extern "C" int sval_get_key_override(uint16_t index, sval_key_override_entry_t *entry) {
    if (index >= SVAL_KEY_OVERRIDE_ENTRIES) return -1;
    *entry = overrides[index];
    return 0;
}

class SvalKeyOverride : public TestFixture {
   public:
    // As Sval 0x06 (sval.c): store the entry, then reload every override.
    void write_override(uint8_t index, uint16_t trigger, uint8_t mods, uint16_t replacement) {
        overrides[index] = {trigger, replacement, 0xFFFFFFFF, mods, 0, mods, (uint8_t)(sval_ko_enabled | sval_ko_option_activation_trigger_down | sval_ko_option_activation_required_mod_down | sval_ko_option_activation_negative_mod_up)};
        sval_reload_key_override();
    }
    void SetUp() override {
        memset(overrides, 0, sizeof(overrides));
        sval_reload_key_override();
    }
};

// Shift+Backspace sends Delete. While it is held a host writes an override:
// Delete comes up, and letting go leaves nothing held.
static void held_override_then(SvalKeyOverride *t, TestDriver &driver, KeymapKey &shift, KeymapKey &bspc, void (*write)(SvalKeyOverride *)) {
    EXPECT_REPORT(driver, (KC_LSFT));
    shift.press();
    t->run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_ANY_REPORT(driver).Times(AnyNumber());
    EXPECT_REPORT(driver, (KC_DEL));
    bspc.press();
    t->run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);

    EXPECT_REPORT(driver, (KC_LSFT));
    write(t);
    VERIFY_AND_CLEAR(driver);

    EXPECT_ANY_REPORT(driver).Times(AnyNumber());
    bspc.release();
    t->run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    shift.release();
    t->run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

TEST_F(SvalKeyOverride, ActiveOverrideReleasesWhenItIsRewritten) {
    TestDriver driver;
    auto       shift = KeymapKey(0, 0, 0, KC_LSFT);
    auto       bspc  = KeymapKey(0, 1, 0, KC_BSPC);
    set_keymap({shift, bspc});
    write_override(0, KC_BSPC, MOD_MASK_SHIFT, KC_DEL);
    held_override_then(this, driver, shift, bspc, [](SvalKeyOverride *t) { t->write_override(0, KC_BSPC, MOD_MASK_SHIFT, KC_ESC); });
}

TEST_F(SvalKeyOverride, ActiveOverrideReleasesWhenItIsDisabled) {
    TestDriver driver;
    auto       shift = KeymapKey(0, 0, 0, KC_LSFT);
    auto       bspc  = KeymapKey(0, 1, 0, KC_BSPC);
    set_keymap({shift, bspc});
    write_override(0, KC_BSPC, MOD_MASK_SHIFT, KC_DEL);
    held_override_then(this, driver, shift, bspc, [](SvalKeyOverride *t) {
        overrides[0] = {};
        sval_reload_key_override();
    });
}

// Writing an unchanged table leaves the active override alone.
TEST_F(SvalKeyOverride, UnchangedTableKeepsTheOverride) {
    TestDriver driver;
    auto       shift = KeymapKey(0, 0, 0, KC_LSFT);
    auto       bspc  = KeymapKey(0, 1, 0, KC_BSPC);
    set_keymap({shift, bspc});
    write_override(0, KC_BSPC, MOD_MASK_SHIFT, KC_DEL);

    EXPECT_REPORT(driver, (KC_LSFT));
    shift.press();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_ANY_REPORT(driver).Times(AnyNumber());
    EXPECT_REPORT(driver, (KC_DEL));
    bspc.press();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);

    EXPECT_NO_REPORT(driver);
    write_override(0, KC_BSPC, MOD_MASK_SHIFT, KC_DEL);
    VERIFY_AND_CLEAR(driver);

    EXPECT_ANY_REPORT(driver).Times(AnyNumber());
    bspc.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
    EXPECT_EMPTY_REPORT(driver);
    shift.release();
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}
