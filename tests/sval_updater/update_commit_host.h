// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The host HAL for update_commit.c (built with -DSVAL_UPDATER_HOST_TEST): what
// the firmware does with a volatile register access, cpsid, an uncached XIP
// read or a jump into boot2, the host does through these mocks (test_commit.c).
// The ROM pointers and the flash die are the staging tests' mocks
// (update_flash_host.h, test_updater.c).

#include <stdbool.h>
#include <stdint.h>
#include "update_flash_host.h"

#define UPDATE_HOST_RAM0_WORDS 4096 // stands in for SRAM0-3 in the reset's magic sweep
#define UPDATE_HOST_INITIAL_SP 0x20041F00u

extern uint32_t update_host_ram0[UPDATE_HOST_RAM0_WORDS];

uint32_t                       update_host_reg_rd(uint32_t addr);
void                           update_host_reg_wr(uint32_t addr, uint32_t v); // a watchdog trigger does not return
uint32_t                       update_host_flash_rd32(uint32_t off);          // checks XIP is on
void                           update_host_primask(bool set);
uint32_t                       update_host_vtor_value(const volatile uintptr_t *table);
bool                           update_host_rom_ptr_ok(void *p);
void __attribute__((noreturn)) update_host_halt(bool fed); // a test-hook halt point
void                           update_host_fault(void);    // a HardFault: runs the vector table's handler
void __attribute__((noreturn)) update_host_spin(void);
