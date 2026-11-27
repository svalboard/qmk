// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Double-tap RESET into the RP2040's USB bootloader (double_tap_reset.h).
//
// Two taps of RESET about 80 to 600 ms apart enter the bootloader; a single
// press never does. The word survives a reset in QMK's noinit section
// (.ram0.bootloader_magic), which nothing else uses once QMK's own double-tap
// option is off.
#include "hal.h"
#include "wait.h"
#include "pico/bootrom.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "double_tap_reset.h"

#ifdef RP2040_BOOTLOADER_DOUBLE_TAP_RESET
#    error "QMK's RP2040 double-tap reset is replaced by double_tap_reset.c; leave it off"
#endif

_Static_assert(DTR_WD_REASON_TIMER == WATCHDOG_REASON_TIMER_BITS && DTR_WD_REASON_FORCE == WATCHDOG_REASON_FORCE_BITS, "watchdog reason bits");
_Static_assert(DTR_CHIP_HAD_RUN == VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS, "CHIP_RESET HAD_RUN");

// The bounce wait: 3 cycles a loop (sub, taken bne). On the ring oscillator,
// which runs the chip until clocks_init(), that is at least 30 ms at its
// fastest (12 MHz) and about 55 ms at its nominal 6.5 MHz.
#ifndef DTR_DEBOUNCE_LOOPS
#    define DTR_DEBOUNCE_LOOPS 120000u
#endif
// How long after the startup code a second tap is still counted.
#ifndef DTR_WINDOW_MS
#    define DTR_WINDOW_MS 500u
#endif

static volatile uint32_t __attribute__((section(".ram0.bootloader_magic"))) dtr_word;

// Runs about 1 ms after RESET is released, before .data and .bss are set up:
// registers and the noinit word only, no calls, no divides (they live in RAM).
void early_hardware_init_pre(void) {
    bool tap = dtr_is_tap(watchdog_hw->reason, vreg_and_chip_reset_hw->chip_reset);
    if (tap) {
        // A bounce resets the chip during this wait, before anything counts.
        uint32_t n = DTR_DEBOUNCE_LOOPS;
        __asm volatile("1: sub %0, #1\n\tbne 1b" : "+l"(n) : : "cc");
    }
    dtr_word = dtr_early_step(dtr_word, tap);
}

// Runs just before main(). A RUN-pin reset leaves the watchdog off, so the
// jump needs no watchdog handling.
void __late_init(void) {
    // The boot ROM needs every clock running before the jump.
    clocks_init();
    if (dtr_late_fires(dtr_word)) {
        dtr_word = 0;
        reset_usb_boot(0, 0);
    }
    // ChibiOS is not running yet: a busy wait.
    wait_us(DTR_WINDOW_MS * 1000u);
    dtr_word = 0;
}
