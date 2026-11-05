// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The link light's rules (keyboards/svalboard/link_light.c) on the host: armed only
// by a pairing in this power session, shown on a loss, cleared when the pairing
// comes back, never after a stop, dark in USB suspend and while an update runs.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

uint32_t host_now_ms;
bool     host_master, host_transport_up, host_other_connected, host_usb_configured, host_updater_active;

#include "link_light.c"

static int checks, failures;
#define CHECK(c)                                                    \
    do {                                                            \
        checks++;                                                   \
        if (!(c)) {                                                 \
            failures++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        }                                                           \
    } while (0)

static uint32_t boot_ms = 1000;

static void boot(bool master) {
    armed = lost = cable_paired = cable_lost = sub_paired = sub_quiet = stop_wait = stop_down = false;
    lost_at = sub_last_ping = stopped_at = 0;
    host_now_ms         = boot_ms;
    host_master         = master;
    host_transport_up   = true; // QMK's check reads connected until 200 ms of failures
    host_other_connected = false;
    host_usb_configured = master;
    host_updater_active = false;
}

static void run_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        host_now_ms++;
        sval_link_task();
    }
}

// The half with USB, over the split cable.
static void test_master(void) {
    // alone after boot: the transport check fires, but nothing was ever paired
    boot(true);
    run_ms(100);
    host_transport_up = false;
    run_ms(2000);
    CHECK(!sval_link_light());

    // paired, then the link drops: light; back: cleared at the first good exchange
    boot(true);
    host_other_connected = true;
    run_ms(10);
    CHECK(!sval_link_light());
    host_transport_up    = false;
    host_other_connected = false;
    run_ms(1);
    CHECK(sval_link_light());
    host_usb_configured = false; // USB suspend: dark, still lost
    run_ms(1);
    CHECK(!sval_link_light());
    host_usb_configured = true;
    host_updater_active = true; // an update restarts halves on purpose
    CHECK(!sval_link_light());
    host_updater_active = false;
    CHECK(sval_link_light());
    host_transport_up = true;
    run_ms(1);
    CHECK(!sval_link_light());
    // and it arms again: the next drop shows again
    host_other_connected = true;
    run_ms(1);
    host_transport_up = false;
    run_ms(1);
    CHECK(sval_link_light());

    // told to stop: no light, and nothing armed until paired again
    sval_link_stopped();
    CHECK(!sval_link_light());
    sval_link_lost();
    CHECK(!sval_link_light());
}

// A restart on purpose (shutdown_kb, a relayed restart of the other half).
static void test_master_stop(void) {
    // the other half restarts: the link goes down, no light; it comes back and arms again
    boot(true);
    host_other_connected = true;
    run_ms(10);
    sval_link_stopped();
    host_transport_up    = false;
    host_other_connected = false;
    run_ms(2500);
    CHECK(!sval_link_light());
    host_transport_up    = true;
    host_other_connected = true;
    run_ms(10);
    CHECK(armed);
    host_transport_up    = false;
    host_other_connected = false;
    run_ms(1);
    CHECK(sval_link_light()); // a real loss after that shows

    // a restart that never happened: armed again after SVAL_LINK_REARM_MS
    boot(true);
    host_other_connected = true;
    run_ms(10);
    sval_link_stopped();
    run_ms(SVAL_LINK_REARM_MS - 20);
    CHECK(!armed);
    run_ms(40);
    CHECK(armed);
    host_transport_up = false;
    run_ms(1);
    CHECK(sval_link_light());
}

static void test_sub_stop(void) {
    // the half with USB restarts on purpose (its last ping said so): no light; its
    // next ping pairs again, and a loss after that shows
    boot(false);
    sval_link_ping_seen(false);
    run_ms(500);
    sval_link_stopped();
    run_ms(5000);
    CHECK(!sval_link_light());
    sval_link_ping_seen(false);
    CHECK(armed);
    run_ms(SVAL_LINK_SUB_LOST_MS + 1);
    CHECK(sval_link_light());
}

// The half without USB: pings every 500 ms.
static void test_sub(void) {
    boot(false);
    run_ms(5000); // no ping ever: not armed
    CHECK(!sval_link_light());

    boot(false);
    for (int i = 0; i < 6; i++) {
        sval_link_ping_seen(false);
        run_ms(500);
    }
    CHECK(!sval_link_light());
    run_ms(SVAL_LINK_SUB_LOST_MS - 500); // silent: still within the allowance
    CHECK(!sval_link_light());
    run_ms(1);
    CHECK(sval_link_light());
    CHECK(timer_elapsed32(sub_last_ping) == SVAL_LINK_SUB_LOST_MS + 1);
    sval_link_ping_seen(false); // the link is back
    CHECK(!sval_link_light());

    // the half with USB in USB suspend: its pings say so, and a loss stays dark
    sval_link_ping_seen(true);
    run_ms(5000);
    CHECK(!sval_link_light());
    sval_link_ping_seen(false);
    run_ms(SVAL_LINK_SUB_LOST_MS + 1);
    CHECK(sval_link_light());
}

int main(void) {
    for (int pass = 0; pass < 2; pass++) {
        boot_ms = pass ? UINT32_MAX - 3000 : 1000; // and once across the 32-bit timer wrap
        test_master();
        test_master_stop();
        test_sub();
        test_sub_stop();
    }
    printf("link light: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
