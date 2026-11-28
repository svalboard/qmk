// SPDX-License-Identifier: GPL-2.0-or-later
// VIA's stored layout stamp and keycode version (quantum/via.c, via_init; a
// Svalboard patch to core). The keyboard's own keycodes outside the VIA region
// (the Sval tables: combos, tap dances, overrides, alt repeat, leader) are
// reset whenever the stored keycode version is not the firmware's, also when
// the layout stamp changed in the same update. Otherwise the stamp path wrote
// the new version and the tables kept the old numbering.
#include "test_common.hpp"
extern "C" {
#include "via.h"
#include "eeconfig.h"
#include "nvm_via.h"
}

static int      keycode_resets;
extern "C" void via_keycodes_reset_kb(void) {
    keycode_resets++;
}

class SvalViaStamp : public TestFixture {
   protected:
    void SetUp() override {
        via_init(); // a valid store with this firmware's stamp and keycode version
        keycode_resets = 0;
    }
    uint8_t current_version() {
        return nvm_via_read_keycodes_version();
    }
};

TEST_F(SvalViaStamp, SameStampSameVersionKeepsEverything) {
    via_init();
    EXPECT_EQ(keycode_resets, 0);
    EXPECT_TRUE(via_eeprom_is_valid());
}

TEST_F(SvalViaStamp, NewVersionResetsKeyboardKeycodes) {
    uint8_t v = current_version();
    nvm_via_update_keycodes_version(v - 1);
    via_init();
    EXPECT_EQ(keycode_resets, 1);
    EXPECT_EQ(current_version(), v);
    EXPECT_TRUE(via_eeprom_is_valid());
}

TEST_F(SvalViaStamp, NewStampAloneKeepsKeyboardKeycodes) {
    via_eeprom_set_valid(false); // the stamp's inputs changed
    via_init();
    EXPECT_EQ(keycode_resets, 0);
    EXPECT_TRUE(via_eeprom_is_valid());
}

TEST_F(SvalViaStamp, NewStampAndNewVersionResetsKeyboardKeycodes) {
    uint8_t v = current_version();
    via_eeprom_set_valid(false);
    nvm_via_update_keycodes_version(v - 1);
    via_init();
    EXPECT_EQ(keycode_resets, 1);
    EXPECT_EQ(current_version(), v);
    EXPECT_TRUE(via_eeprom_is_valid());
    via_init(); // and once only
    EXPECT_EQ(keycode_resets, 1);
}
