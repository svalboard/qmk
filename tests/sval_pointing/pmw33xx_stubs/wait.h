// Host-test stand-in for QMK's wait.h (pmw33xx_burst_test.c): waits cost nothing.
#pragma once
#include <stdint.h>
static inline void wait_us(uint32_t us) {
    (void)us;
}
static inline void wait_ms(uint32_t ms) {
    (void)ms;
}
