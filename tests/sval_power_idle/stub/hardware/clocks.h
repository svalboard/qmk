// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// pico-sdk hardware/clocks.h, as far as power.c uses it (test_clock_switch.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum clock_index { clk_gpout0, clk_gpout1, clk_gpout2, clk_gpout3, clk_ref, clk_sys, clk_peri, clk_usb, clk_adc, clk_rtc };
#define CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX 1u
#define CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS 0u
#define CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB 1u
#define CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_XOSC_CLKSRC 5u
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS 0u
bool clock_configure(enum clock_index clk, uint32_t src, uint32_t auxsrc, uint32_t src_freq, uint32_t freq);
