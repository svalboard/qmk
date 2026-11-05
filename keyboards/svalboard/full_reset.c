// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Every firmware restart is a full chip reset.
//
// QMK's RP2040 mcu_reset() (platforms/chibios/bootloaders/rp2040.c) is
// NVIC_SystemReset(). On the RP2040 that resets the processor cores only: the
// peripherals keep running into the next boot. The split link's PIO state
// machines keep driving their pins (and the half with USB's receive DMA keeps
// writing its ring), and the USB controller keeps its state. A half that
// restarts with the split cable in can then come up unable to link, or, with
// USB plugged in, unable to enumerate, until it is powered off.
//
// Everything that restarts the firmware on purpose goes through mcu_reset():
// soft_reset_keyboard() (a side write, a rename, Scan Lab, the link light's
// tests) and QMK's split watchdog on the half without USB. Here it resets the
// whole chip through the watchdog instead, as a power-up does, except for the
// oscillators (as the SDK's watchdog_reboot() does) and the watchdog's scratch
// registers, which the updater's crumbs use. The updater's commit has its own
// reset (update_commit.c) and the bootloader jump its own (reset_usb_boot()).
#include <stdint.h>
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"

#define PSM_WDSEL_ALL_BUT_OSC 0x0001FFFCu // everything but ROSC and XOSC (update_commit.c)

void mcu_reset(void) {
    // Scratch 4 holds the boot ROM's "start at an address after a watchdog reset"
    // magic; a stale one would send the reset somewhere else.
    watchdog_hw->scratch[4] = 0;
    psm_hw->wdsel           = PSM_WDSEL_ALL_BUT_OSC;
    watchdog_hw->ctrl       = WATCHDOG_CTRL_TRIGGER_BITS;
    while (1) {
    }
}
