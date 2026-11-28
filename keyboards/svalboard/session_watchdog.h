// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The RP2040 hardware watchdog while the updater is active (session_watchdog.c).
//
// The board runs without a hardware watchdog, so a half whose update session
// hung (a stuck flash operation, a lost loop, anything that stops the main
// loop) stayed hung until it was unplugged or reset by hand. While
// updater_active(), the watchdog now runs with an 8 s period and is fed only
// where the updater makes progress (updater_port_alive(), updater_port.h): a
// main-loop pass, a host request, a request from the other half, a sector of
// the commit's slot check, and a 64 KiB unit of a settings consolidation's
// erase. A half that stops for 8 s resets and boots the firmware it has.
// Outside a session the watchdog is off, as before.
#pragma once

#include <stdbool.h>

// The period. The counter drops 2 per microsecond (RP2040 erratum E1), so
// LOAD is twice this in us, and LOAD is 24 bits: at most 8.39 s.
#define SESSION_WATCHDOG_MS 8000u

// Reloads the counter if the watchdog runs; never turns it on.
void session_watchdog_feed(void);
// Off (the board's normal state): before the boot ROM's USB bootloader,
// which does not feed it, and while USB is suspended (the main loop does not
// run the updater then).
void session_watchdog_off(void);
bool session_watchdog_running(void);
