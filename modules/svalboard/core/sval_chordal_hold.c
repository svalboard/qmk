// SPDX-License-Identifier: GPL-2.0-or-later
// Chordal Hold's hand for each key position.

#include "quantum.h"

#ifdef CHORDAL_HOLD

// As QMK's own chordal_hold_handedness(), from chordal_hold_layout, except a
// position outside the matrix belongs to neither hand ('*'). Tap dances,
// macros and combos send their keycodes as key events at row 240, column 240
// (sval.c), and QMK's version would read that position from far past the end
// of the layout.
char chordal_hold_handedness(keypos_t key) {
    if (key.row >= MATRIX_ROWS || key.col >= MATRIX_COLS) return '*';
    return (char)pgm_read_byte(&chordal_hold_layout[key.row][key.col]);
}

#endif
