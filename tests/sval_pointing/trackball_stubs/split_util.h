// Host-test stand-in for QMK's split_util.h, only what squal.c uses: a left half with USB.
#pragma once
#include <stdbool.h>
static inline bool is_keyboard_left(void) {
    return true;
}
static inline bool is_keyboard_master(void) {
    return true;
}
