// SPDX-License-Identifier: GPL-2.0-or-later
// Chordal Hold and the module's key events from row 240, column 240 (sval.c):
// that position belongs to neither hand and is never read from the layout.
#include "test_common.hpp"

class SvalChordalHold : public TestFixture {};

static keyrecord_t key_at(uint8_t row, uint8_t col) {
    keyrecord_t record   = {};
    record.event.key     = {.col = col, .row = row};
    record.event.type    = KEY_EVENT;
    record.event.pressed = true;
    record.event.time    = 1;
    return record;
}

TEST_F(SvalChordalHold, PositionsOutsideTheMatrixAreNeitherHand) {
    EXPECT_EQ(chordal_hold_handedness({.col = 240, .row = 240}), '*');
    EXPECT_EQ(chordal_hold_handedness({.col = 0, .row = MATRIX_ROWS}), '*');
    EXPECT_EQ(chordal_hold_handedness({.col = MATRIX_COLS, .row = 0}), '*');
}

TEST_F(SvalChordalHold, MatrixPositionsComeFromTheLayout) {
    EXPECT_EQ(chordal_hold_handedness({.col = 0, .row = 0}), 'L');
    EXPECT_EQ(chordal_hold_handedness({.col = 4, .row = 3}), 'L');
    EXPECT_EQ(chordal_hold_handedness({.col = 5, .row = 0}), 'R');
    EXPECT_EQ(chordal_hold_handedness({.col = 9, .row = 3}), 'R');
}

// A left-hand mod-tap and a key a tap dance sends: the chord may settle as a
// hold, as with a key from the other hand. A key from the same hand still may not.
TEST_F(SvalChordalHold, TapDanceKeyDoesNotForceATap) {
    keyrecord_t mod_tap = key_at(0, 0);
    keyrecord_t magic   = key_at(240, 240);
    keyrecord_t same    = key_at(1, 1);
    keyrecord_t other   = key_at(1, 6);
    EXPECT_TRUE(get_chordal_hold_default(&mod_tap, &magic));
    EXPECT_TRUE(get_chordal_hold_default(&magic, &mod_tap));
    EXPECT_TRUE(get_chordal_hold_default(&mod_tap, &other));
    EXPECT_FALSE(get_chordal_hold_default(&mod_tap, &same));
}
