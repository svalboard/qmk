// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The link-loss light (link_light.h, docs/link-light.md).
#ifdef SVAL_LINK_HOST_TEST
#    include "link_light_host.h" // tests/sval_link: timer, split and USB mocks
#else
#    include "quantum.h"
#    include "split_common/split_util.h"
#    include "split_common/transactions.h"
#    include "usb_device_state.h"
#    include "svalboard.h"
#    ifdef SVAL_UPDATER
#        include "updater/updater.h" // updater_active
#    endif
#endif
#include "link_light.h"
#include <string.h>

// The other half: pings come a little over every 500 ms (svalboard.c), so
// 1.5 s is two missed pings and most of a third.
#ifndef SVAL_LINK_SUB_LOST_MS
#    define SVAL_LINK_SUB_LOST_MS 1500
#endif
// The half with USB after a stop on purpose: it pairs again once the link has gone
// down and up, or after this long if the link never went down (a restart that did
// not happen).
#ifndef SVAL_LINK_REARM_MS
#    define SVAL_LINK_REARM_MS 5000
#endif

static bool     armed, lost;
static uint32_t lost_at;

// the half with USB
static bool cable_paired; // a presence ping answered since boot or the last loss
static bool cable_lost;   // the transport check fired while paired
static bool     stop_wait;   // stopped on purpose: wait for the link to go down (or the delay)
static bool     stop_down;   // the link went down since the stop
static uint32_t stopped_at;
// the other half
static bool     sub_paired, sub_quiet;
static uint32_t sub_last_ping;

#ifdef SVAL_LINK_TEST
static uint16_t last_gap_ms;     // the other half: silence when it called the loss
static uint32_t last_lost_ms;    // how long the last loss lasted, to its clear
static uint8_t  losses, clears;  // counters since boot
#endif

void sval_link_paired(void) {
    bool was_lost = lost;
#ifdef SVAL_LINK_TEST
    if (lost) {
        last_lost_ms = timer_elapsed32(lost_at);
        clears++;
    }
#endif
    armed = true;
    lost  = false;
    if (was_lost) sval_link_recovered();
}

void sval_link_lost(void) {
    if (!armed || lost) return;
    lost    = true;
    lost_at = timer_read32();
#ifdef SVAL_LINK_TEST
    losses++;
#endif
}

void sval_link_stopped(void) {
    armed        = false;
    lost         = false;
    cable_paired = false;
    cable_lost   = false;
    sub_paired   = false;
    stop_wait    = true;
    stop_down    = false;
    stopped_at   = timer_read32();
}

void sval_link_ping_seen(bool quiet) {
    sub_last_ping = timer_read32();
    sub_quiet     = quiet;
    if (!sub_paired) {
        sub_paired = true;
        sval_link_paired();
    }
}

// The half with USB after a stop on purpose: true while it should not pair yet.
static bool stop_holds(bool up) {
    if (!stop_wait) return false;
    if (!up || !sval_other_half_connected()) stop_down = true;
    if ((stop_down && up && sval_other_half_connected()) || timer_elapsed32(stopped_at) >= SVAL_LINK_REARM_MS) stop_wait = false;
    return stop_wait;
}

void sval_link_task(void) {
    if (is_keyboard_master()) {
        bool up = is_transport_connected();
        if (stop_holds(up)) {
            // not yet: the restart asked for is still going on
        } else if (!cable_paired && !cable_lost && sval_other_half_connected()) {
            cable_paired = true;
            sval_link_paired();
        } else if (cable_paired && !up) {
            cable_paired = false;
            cable_lost   = true;
            sval_link_lost();
        } else if (cable_lost && up) { // the first good exchange after the loss
            cable_lost   = false;
            cable_paired = true;
            sval_link_paired();
        }
    } else if (sub_paired && !sub_quiet && timer_elapsed32(sub_last_ping) > SVAL_LINK_SUB_LOST_MS) {
#ifdef SVAL_LINK_TEST
        uint32_t gap = timer_elapsed32(sub_last_ping);
        last_gap_ms  = gap > 0xFFFF ? 0xFFFF : (uint16_t)gap;
#endif
        sub_paired = false;
        sval_link_lost();
    }
#ifdef SVAL_LINK_TEST
    sval_link_test_task();
#endif
}

bool sval_link_quiet(void) {
    return usb_device_state_get_configure_state() != USB_DEVICE_STATE_CONFIGURED;
}

bool sval_link_light(void) {
    if (!lost) return false;
#ifdef SVAL_UPDATER
    // An update restarts halves on purpose: no link light while one runs.
    if (updater_active()) return false;
#endif
    if (is_keyboard_master()) return !sval_link_quiet();
    return !sub_quiet;
}

// ---- dev test hooks ---------------------------------------------------------------

#ifdef SVAL_LINK_TEST
#    include "usb_util.h"
#    include "split_pause.h"
#    include "updater/update_led.h"
#    include "hardware/structs/watchdog.h"

// Wire, after [0xDD][client:4][0xFA]: [op] ...; the reply keeps the header and op.
//   0 STATUS: [1] this half, [11] the other half over the link (zero when unreachable)
//   1 PAUSE ms:2 (the half with USB): the split exchange and the pings stop for that
//     long, without telling the other half, as a broken link would look to it
//   2 SUB REBOOT [normal] (the half with USB): the other half restarts, as the half
//     without USB again (normal 1: as it decides itself), which the half with USB
//     sees as a broken link until it is back
//   3 BOOT AS SUB: this half restarts as the half without USB, once (the next
//     restart is normal again; with no other half answering, the split watchdog
//     restarts it within a few seconds)
// A status block: flags (bit 0 armed, 1 lost, 2 light now, 3 USB half, 4 cable paired,
// 5 quiet), ms since the loss:2, the other half's silence when it called its last
// loss:2, the last loss's length:2, losses, clears, 0, then the LED: what it shows
// (r g b) and what rgblight last asked for (r g b).
enum { LT_STATUS = 0, LT_PAUSE = 1, LT_SUB_REBOOT = 2, LT_BOOT_AS_SUB = 3 };
#    define LT_RPC_OP 0xB1 // first byte of the KEYBOARD_SYNC_B request: no Scan Lab, side or flicker op uses it
#    define LT_STATUS_LEN 16
#    define LT_SUB_MAGIC 0x5B5B1E7Eu
#    define LT_SCRATCH 7 // watchdog scratch 7: the boot ROM reads it only with its own magic in scratch 4

static uint32_t pause_until;
static bool     pausing, reboot_pending;
static uint32_t reboot_at;

static void put16(uint8_t *p, uint32_t v) {
    if (v > 0xFFFF) v = 0xFFFF;
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void status_block(uint8_t *b) {
    b[0] = (armed ? 1 : 0) | (lost ? 2 : 0) | (sval_link_light() ? 4 : 0) | (is_keyboard_master() ? 8 : 0) | (cable_paired ? 16 : 0) | (sub_quiet ? 32 : 0);
    put16(&b[1], lost ? timer_elapsed32(lost_at) : 0);
    put16(&b[3], last_gap_ms);
    put16(&b[5], last_lost_ms);
    b[7] = losses;
    b[8] = clears;
    b[9] = 0;
    update_led_readback(&b[10]); // the LED: shown r g b, rgblight's r g b
}

void sval_link_test_command(uint8_t *data, uint8_t length) {
    if (length < 32) return;
    uint8_t op = data[6];
    uint8_t st = 0;
    switch (op) {
        case LT_STATUS: {
            memset(&data[7], 0, length - 7);
            status_block(&data[8]);
            if (is_keyboard_master() && is_transport_connected() && !sval_split_paused()) {
                uint8_t req[2] = {LT_RPC_OP, LT_STATUS};
                uint8_t rsp[LT_STATUS_LEN + 1];
                if (transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(rsp), rsp) && rsp[0] == LT_RPC_OP) {
                    memcpy(&data[8 + LT_STATUS_LEN], &rsp[1], LT_STATUS_LEN);
                    data[7] |= 1; // the other half's block is there
                }
            }
            return;
        }
        case LT_PAUSE:
            // Never during an update: it owns the pause, and this would end it.
            if (!is_keyboard_master() || pausing || sval_split_paused() || updater_active() || !sval_split_pause(true)) {
                st = 1;
                break;
            }
            pausing     = true;
            pause_until = timer_read32() + (data[7] | (uint32_t)data[8] << 8);
            break;
        case LT_SUB_REBOOT: {
            uint8_t req[3] = {LT_RPC_OP, LT_SUB_REBOOT, data[7]}; // [7] 1: back as itself, not as the sub
            uint8_t rsp[2] = {0};
            if (!is_keyboard_master() || !transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(rsp), rsp) || rsp[0] != LT_RPC_OP) st = 1;
            break;
        }
        case LT_BOOT_AS_SUB:
            watchdog_hw->scratch[LT_SCRATCH] = LT_SUB_MAGIC;
            reboot_pending                   = true;
            reboot_at                        = timer_read32() + 100; // reply first
            break;
        default:
            st = 2;
            break;
    }
    memset(&data[7], 0, length - 7);
    data[7] = st;
}

bool sval_link_test_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 2 || in[0] != LT_RPC_OP || out_len < 2) return false;
    out[0] = LT_RPC_OP;
    if (in[1] == LT_STATUS && out_len >= LT_STATUS_LEN + 1) {
        status_block(&out[1]);
    } else if (in[1] == LT_SUB_REBOOT) {
        // Back as the half without USB, as a half powered over the cable would be,
        // even when a test has its USB plugged in too (unless asked for a normal restart).
        if (!(in_len >= 3 && in[2] == 1)) watchdog_hw->scratch[LT_SCRATCH] = LT_SUB_MAGIC;
        reboot_pending                   = true;
        reboot_at      = timer_read32() + 50;
        out[1]         = 0;
    }
    return true;
}

void sval_link_test_task(void) {
    if (pausing && timer_expired32(timer_read32(), pause_until)) {
        pausing = false;
        sval_split_pause(false);
    }
    if (reboot_pending && timer_expired32(timer_read32(), reboot_at)) soft_reset_keyboard();
}

// BOOT AS SUB: this restart only, decide "no USB" whatever the USB says. Otherwise
// as QMK's own (quantum/split_common/split_util.c, SPLIT_USB_DETECT).
bool is_keyboard_master_impl(void) {
    bool as_sub = watchdog_hw->scratch[LT_SCRATCH] == LT_SUB_MAGIC;
    watchdog_hw->scratch[LT_SCRATCH] = 0;
    bool is_master = false;
    if (!as_sub) {
#    ifndef SPLIT_USB_TIMEOUT
#        define SPLIT_USB_TIMEOUT 2000
#    endif
#    ifndef SPLIT_USB_TIMEOUT_POLL
#        define SPLIT_USB_TIMEOUT_POLL 10
#    endif
        for (uint16_t i = 0; i < SPLIT_USB_TIMEOUT / SPLIT_USB_TIMEOUT_POLL && !is_master; i++) {
            if (usb_connected_state()) {
                is_master = true;
            } else {
                wait_ms(SPLIT_USB_TIMEOUT_POLL);
            }
        }
    }
    if (!is_master) usb_disconnect();
    return is_master;
}
#endif
