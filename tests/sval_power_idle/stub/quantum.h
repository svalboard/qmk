// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Mocks for keyboards/svalboard/power.c on the host (test_clock_switch.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t timer_read32(void);
uint32_t timer_elapsed32(uint32_t t);
bool     is_keyboard_master(void);
