// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Double-tap RESET into the RP2040's USB bootloader (double_tap_reset.c).
//
// QMK's RP2040_BOOTLOADER_DOUBLE_TAP_RESET arms its window in __late_init,
// after the startup code has copied .data and zeroed .bss on the ring
// oscillator. On an image this size that takes a few hundred ms, so a quick
// second tap lands before the window opens and only resets the board.
//
// This version arms early, in early_hardware_init_pre, about 50 ms after
// RESET is released (the wait lets contact bounce reset the chip before
// anything is counted), and still decides in __late_init. Only a reset from
// the RUN pin counts as a tap: a watchdog reset (every firmware restart, the
// updater's commit, the boot ROM's restart after a UF2 copy), a power-up or a
// debugger restart clears the word, so a software reboot can never be taken
// for a tap.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The word's values. ARMED is QMK's double-tap token, which the updater's
// commit already clears from RAM (update_commit.c).
#define DTR_ARMED 0xCAFEB0BAu
#define DTR_FIRE 0xB007F17Eu

// Reset reason bits (RP2040 WATCHDOG_REASON and CHIP_RESET; checked against
// the SDK headers in double_tap_reset.c).
#define DTR_WD_REASON_TIMER 0x00000001u
#define DTR_WD_REASON_FORCE 0x00000002u
#define DTR_CHIP_HAD_RUN 0x00010000u

// A reset counts as a tap only when it came from the RUN pin: the watchdog
// logged no reason (a RUN-pin reset clears it) and the chip says RUN.
static inline bool dtr_is_tap(uint32_t wd_reason, uint32_t chip_reset) {
    return (wd_reason & (DTR_WD_REASON_TIMER | DTR_WD_REASON_FORCE)) == 0 && (chip_reset & DTR_CHIP_HAD_RUN) != 0;
}

// The word after early init: a tap toggles ARMED and FIRE (anything else
// becomes ARMED); any other reset clears it.
static inline uint32_t dtr_early_step(uint32_t word, bool tap) {
    if (!tap) return 0;
    return word == DTR_ARMED ? DTR_FIRE : DTR_ARMED;
}

// Whether __late_init enters the bootloader.
static inline bool dtr_late_fires(uint32_t word) {
    return word == DTR_FIRE;
}
