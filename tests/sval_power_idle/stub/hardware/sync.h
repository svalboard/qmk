// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// pico-sdk hardware/sync.h, as far as power.c uses it (test_clock_switch.c).
#pragma once
#include <stdint.h>

uint32_t save_and_disable_interrupts(void);
void     restore_interrupts(uint32_t status);
