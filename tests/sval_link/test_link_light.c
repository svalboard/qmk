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
void (*host_on_timer)(void);

#include "link_light.c"

// svalboard.c: repaints and resyncs the lighting when a loss clears.
static int recovered;
void       sval_link_recovered(void) {
    recovered++;
}

static int checks, failures;
#define CHECK(c)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(c)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                       \
    } while (0)

static uint32_t boot_ms = 1000;

static void boot(bool master) {
    armed = lost = cable_paired = cable_lost = sub_paired = sub_quiet = sub_busy = sub_ping_due = sub_stop_due = stop_wait = stop_down = false;
    lost_at = sub_last_ping = stopped_at = 0;
    host_on_timer                        = NULL;
    host_now_ms                          = boot_ms;
    host_master                          = master;
    host_transport_up                    = true; // QMK's check reads connected until 200 ms of failures
    host_other_connected                 = false;
    host_usb_configured                  = master;
    host_updater_active                  = false;
    recovered                            = 0;
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
    CHECK(recovered == 0); // a first pairing is no recovery
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
    int before        = recovered;
    host_transport_up = true;
    run_ms(1);
    CHECK(!sval_link_light());
    CHECK(recovered == before + 1); // the lighting is repainted and resent once
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
    sval_link_ping_seen(0);
    run_ms(500);
    sval_link_ping_seen(SVAL_LINK_QUIET_STOPPING);
    CHECK(armed); // noted only: the main loop stops it
    run_ms(1);
    CHECK(!armed);
    run_ms(5000);
    CHECK(!sval_link_light());
    sval_link_ping_seen(0);
    run_ms(1);
    CHECK(armed);
    run_ms(SVAL_LINK_SUB_LOST_MS + 1);
    CHECK(sval_link_light());

    // a ping and then the STOPPING ping before the main loop runs: stopped
    boot(false);
    sval_link_ping_seen(0);
    run_ms(500);
    sval_link_ping_seen(0);
    sval_link_ping_seen(SVAL_LINK_QUIET_STOPPING);
    run_ms(1);
    CHECK(!armed);
    run_ms(5000);
    CHECK(!sval_link_light());
}

// The presence ping runs on the SlaveThread, which preempts the main loop: it
// only notes the ping, and the main loop's pass pairs from it.
static void test_sub_ping_thread(void) {
    boot(false);
    sval_link_ping_seen(0);
    run_ms(500);
    run_ms(SVAL_LINK_SUB_LOST_MS);
    CHECK(sval_link_light());
    int before = recovered;
    sval_link_ping_seen(0); // on the SlaveThread: no pairing state changes there
    CHECK(lost);
    CHECK(recovered == before);
    run_ms(1); // the main loop's next pass
    CHECK(!lost);
    CHECK(recovered == before + 1);
}

// The link comes back exactly at the 1.5 s mark: the ping lands inside the main
// loop's loss check, at each timer read in it. Whichever, the pass after the ping
// shows no light, and a real silence later still shows it.
static int  ping_at, timer_reads;
static void ping_now(void) {
    if (timer_reads++ == ping_at) {
        host_on_timer = NULL; // the ping's own timer read
        sval_link_ping_seen(0);
    }
}
static void test_sub_ping_race(void) {
    for (ping_at = 0; ping_at < 4; ping_at++) {
        boot(false);
        sval_link_ping_seen(0);
        run_ms(1);
        run_ms(SVAL_LINK_SUB_LOST_MS - 1); // the next pass calls the loss
        timer_reads   = 0;
        host_on_timer = ping_now;
        run_ms(1);
        if (host_on_timer) { // fewer timer reads than that: the ping lands just after the pass
            host_on_timer = NULL;
            sval_link_ping_seen(0);
        }
        run_ms(1);
        CHECK(!sval_link_light());
        CHECK(sub_paired);
        if (sval_link_light() || !sub_paired) printf("  ping at timer read %d\n", ping_at);
        run_ms(SVAL_LINK_SUB_LOST_MS + 1); // and no ping since: a real loss
        CHECK(sval_link_light());
    }
}

// A settings consolidation on the half with USB stops its main loop for a few
// seconds: its BUSY ping before each erase allows SVAL_LINK_SUB_BUSY_MS of silence.
static void test_sub_busy(void) {
    boot(false);
    sval_link_ping_seen(0);
    run_ms(500);
    sval_link_ping_seen(SVAL_LINK_QUIET_BUSY);
    run_ms(SVAL_LINK_SUB_BUSY_MS);
    CHECK(!sval_link_light());
    sval_link_ping_seen(0); // the consolidation is done: plain pings again
    run_ms(SVAL_LINK_SUB_LOST_MS);
    CHECK(!sval_link_light());
    run_ms(1);
    CHECK(sval_link_light()); // and the plain allowance applies again

    // the half with USB stops for good during a consolidation: the light shows
    // once the longer allowance is over
    boot(false);
    sval_link_ping_seen(0);
    run_ms(500);
    sval_link_ping_seen(SVAL_LINK_QUIET_BUSY);
    run_ms(SVAL_LINK_SUB_BUSY_MS + 1);
    CHECK(sval_link_light());
}

// The half without USB: pings every 500 ms.
static void test_sub(void) {
    boot(false);
    run_ms(5000); // no ping ever: not armed
    CHECK(!sval_link_light());

    boot(false);
    for (int i = 0; i < 6; i++) {
        sval_link_ping_seen(0);
        run_ms(500);
    }
    CHECK(!sval_link_light());
    run_ms(SVAL_LINK_SUB_LOST_MS - 500); // silent: still within the allowance
    CHECK(!sval_link_light());
    run_ms(1);
    CHECK(sval_link_light());
    CHECK(timer_elapsed32(sub_last_ping) == SVAL_LINK_SUB_LOST_MS + 1);
    sval_link_ping_seen(0); // the link is back
    run_ms(1);
    CHECK(!sval_link_light());

    // the half with USB in USB suspend: its pings say so, and a loss stays dark
    sval_link_ping_seen(SVAL_LINK_QUIET_SUSPEND);
    run_ms(5000);
    CHECK(!sval_link_light());
    sval_link_ping_seen(0);
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
        test_sub_ping_thread();
        test_sub_ping_race();
        test_sub_busy();
    }
    printf("link light: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
