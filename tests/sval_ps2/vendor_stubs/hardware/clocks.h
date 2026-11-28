#pragma once
#define clk_sys 0
static inline unsigned clock_get_hz(int clk) {
    (void)clk;
    return 125000000u;
}
