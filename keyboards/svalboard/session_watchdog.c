// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The RP2040 hardware watchdog while the updater is active (session_watchdog.h).
//
// updater_port_alive() (updater_port.h) is the only thing that turns it on,
// and only while updater_active(); it is fed there and at the two other
// progress points (session_watchdog_feed). Nothing feeds it from an interrupt
// or a timer: a main loop that stops, with no request from the host or the
// other half reaching the updater, lets it fire.
//
// The longest legitimate gap between feeds is one flash erase with interrupts
// off: a 64 KiB block of the staging slot or of the settings store, about
// 0.4 s on these dies and 2 s at the W25Q128JV datasheet maximum, a quarter of
// the period. The commit arms the same watchdog itself for its copy
// (update_commit.c, also 8 s) and resets through it.
//
// A watchdog timeout resets the chip as mcu_reset() does (full_reset.c):
// everything but the oscillators, with scratch 4 cleared so the boot ROM
// boots the flash. The reset reason is "watchdog timer", which the double-tap
// RESET check never counts as a tap (double_tap_reset.h), and the updater's
// session is in RAM, so the half boots its firmware with no session.
#include <stdint.h>
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/sync.h"
#include "session_watchdog.h"
#include "updater/updater.h"
#include "updater/updater_port.h"

#define SESSION_WATCHDOG_LOAD (SESSION_WATCHDOG_MS * 1000u * 2u)
_Static_assert(SESSION_WATCHDOG_LOAD <= WATCHDOG_LOAD_BITS, "the watchdog's LOAD is 24 bits (8.39 s)");
_Static_assert(SESSION_WATCHDOG_MS >= 4u * 2000u, "at least four times the worst 64 KiB block erase (2 s)");

#define PSM_WDSEL_ALL_BUT_OSC 0x0001FFFCu // as full_reset.c and update_commit.c
_Static_assert(PSM_WDSEL_ALL_BUT_OSC == (PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS)), "PSM WDSEL: everything but the oscillators");

// The main thread and the split SlaveThread (the other half's requests) both
// get here: each change is a few register writes with interrupts off.
static volatile bool running;

static void watchdog_on(void) {
    uint32_t irq = save_and_disable_interrupts();
    if (!running) {
        // PSM_WDSEL first: it resets to 0, and a watchdog reset with 0 resets
        // nothing. Scratch 4 holds the boot ROM's "start at an address" magic.
        psm_hw->wdsel           = PSM_WDSEL_ALL_BUT_OSC;
        watchdog_hw->scratch[4] = 0;
        watchdog_hw->load       = SESSION_WATCHDOG_LOAD;
        hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
        running = true;
    }
    restore_interrupts(irq);
}

void session_watchdog_off(void) {
    uint32_t irq = save_and_disable_interrupts();
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    running = false;
    restore_interrupts(irq);
}

void session_watchdog_feed(void) {
    if (running) watchdog_hw->load = SESSION_WATCHDOG_LOAD;
}

bool session_watchdog_running(void) {
    return running;
}

void updater_port_alive(void) {
    if (updater_active()) {
        watchdog_on();
        session_watchdog_feed();
    } else if (running) {
        session_watchdog_off(); // the session ended: back to no watchdog
    }
}

// A settings consolidation erases the whole store, 64 KiB at a time
// (platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash.c):
// each unit erased and read back is progress.
void wear_leveling_rp2040_erased_unit(void) {
    session_watchdog_feed();
}
