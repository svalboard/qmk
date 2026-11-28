// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// pico-sdk hardware/pio.h, as far as power.c uses it (test_clock_switch.c).
#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t ctrl;
    struct {
        volatile uint32_t clkdiv;
    } sm[4];
} pio_hw_t;
typedef pio_hw_t *PIO;
extern pio_hw_t   host_pio[2];
#define pio0 (&host_pio[0])
#define pio1 (&host_pio[1])
#define PIO_CTRL_SM_ENABLE_LSB 0u
