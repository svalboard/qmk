// Host-test stand-in for QMK's keyboard.h (pmw33xx_burst_test.c).
#pragma once
#include <stdbool.h>
static inline bool is_keyboard_left(void) {
    return true;
}
