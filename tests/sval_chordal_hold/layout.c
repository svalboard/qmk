// SPDX-License-Identifier: GPL-2.0-or-later
// chordal_hold_layout: left hand in columns 0-4, right hand in 5-9. It is the
// start of a larger block of 'L', so a read from row 240, column 240 (far past
// its end) gives 'L', the same hand as column 0, every time.
#include "quantum.h"

#define BLOCK_SIZE (240 * MATRIX_COLS + 240 + 1)
#define RIGHT(r) [(r) * MATRIX_COLS + 5] = 'R', [(r) * MATRIX_COLS + 6] = 'R', [(r) * MATRIX_COLS + 7] = 'R', [(r) * MATRIX_COLS + 8] = 'R', [(r) * MATRIX_COLS + 9] = 'R'

_Static_assert(MATRIX_ROWS == 4 && MATRIX_COLS == 10, "the rows below match the test matrix");
static const char block[BLOCK_SIZE] = {
    [0 ... BLOCK_SIZE - 1] = 'L', RIGHT(0), RIGHT(1), RIGHT(2), RIGHT(3),
};

extern const char chordal_hold_layout[MATRIX_ROWS][MATRIX_COLS] __attribute__((alias("block")));
