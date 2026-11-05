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

static void boot(bool master) {
    armed = lost = cable_paired = cable_lost = sub_paired = sub_quiet = false;
    lost_at = sub_last_ping = 0;
    host_now_ms         = 1000;
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
    test_master();
    test_sub();
    printf("link light: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
