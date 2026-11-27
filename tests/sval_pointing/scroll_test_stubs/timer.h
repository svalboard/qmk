// Host stand-in for QMK's timer.h (scroll_test_test.c sets the time).
#pragma once
#include <stdint.h>
extern uint32_t        fake_now;
static inline uint32_t timer_read32(void) {
    return fake_now;
}
#define TIMER_DIFF_32(a, b) ((uint32_t)((a) - (b)))
