// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// pico-sdk hardware/pll.h, as far as power.c uses it (test_clock_switch.c).
#pragma once
#include <stdint.h>

typedef struct host_pll *PLL;
extern struct host_pll   host_pll_sys;
#define pll_sys (&host_pll_sys)
void pll_init(PLL pll, uint32_t refdiv, uint32_t vco_freq, uint32_t post_div1, uint32_t post_div2);
void pll_deinit(PLL pll);
