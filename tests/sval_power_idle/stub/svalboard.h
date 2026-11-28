// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The parts of keyboards/svalboard/svalboard.h that power.c uses (test_clock_switch.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t scan_deep_clock_idx; // 0 = 48 MHz, 1 = 24 MHz, 2 = 12 MHz
} saved_values_t;
extern saved_values_t global_saved_values;

#define SVAL_DEEP_NAP_MASTER_US 20000
#define SVAL_DEEP_NAP_SLAVE_US 4000

uint8_t  sval_deep_clock_mhz(void);
bool     sval_clock_is_low(void);
uint8_t  sval_clock_mhz(void);
void     sval_clock_low(void);
void     sval_clock_full(void);
uint32_t sval_deep_nap_us(void);
void     sval_sleep_gating_init(void);
