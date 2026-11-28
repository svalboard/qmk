// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// pico-sdk hardware/structs/clocks.h, as far as power.c uses it (test_clock_switch.c).
#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t sleep_en0, sleep_en1;
} clocks_hw_t;
extern clocks_hw_t host_clocks;
#define clocks_hw (&host_clocks)
static inline void hw_clear_bits(volatile uint32_t *addr, uint32_t mask) {
    *addr &= ~mask;
}
#define CLOCKS_SLEEP_EN0_CLK_SYS_JTAG_BITS (1u << 0)
#define CLOCKS_SLEEP_EN0_CLK_SYS_ADC_BITS (1u << 1)
#define CLOCKS_SLEEP_EN0_CLK_ADC_ADC_BITS (1u << 2)
#define CLOCKS_SLEEP_EN0_CLK_SYS_PWM_BITS (1u << 3)
#define CLOCKS_SLEEP_EN0_CLK_SYS_I2C0_BITS (1u << 4)
#define CLOCKS_SLEEP_EN0_CLK_SYS_I2C1_BITS (1u << 5)
#define CLOCKS_SLEEP_EN0_CLK_SYS_RTC_BITS (1u << 6)
#define CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS (1u << 7)
#define CLOCKS_SLEEP_EN0_CLK_SYS_SPI0_BITS (1u << 8)
#define CLOCKS_SLEEP_EN0_CLK_PERI_SPI0_BITS (1u << 9)
#define CLOCKS_SLEEP_EN0_CLK_SYS_SPI1_BITS (1u << 10)
#define CLOCKS_SLEEP_EN0_CLK_PERI_SPI1_BITS (1u << 11)
#define CLOCKS_SLEEP_EN1_CLK_SYS_TBMAN_BITS (1u << 0)
#define CLOCKS_SLEEP_EN1_CLK_SYS_UART0_BITS (1u << 1)
#define CLOCKS_SLEEP_EN1_CLK_PERI_UART0_BITS (1u << 2)
#define CLOCKS_SLEEP_EN1_CLK_SYS_UART1_BITS (1u << 3)
#define CLOCKS_SLEEP_EN1_CLK_PERI_UART1_BITS (1u << 4)
