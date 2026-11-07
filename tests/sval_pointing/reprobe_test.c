// Host test for the pointing module search (keyboards/svalboard/pointer_reprobe.c): the
// schedule (1.5, 3, 6, 12 and 30 s, then never again), one bus per main loop pass, starting
// what's found, stopping after a find, and never probing a half whose module works. The clock, the half's identity, the buses
// and the drivers are mocks; each test runs in its own process (main forks per test).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "reprobe_test_hal.h"
#include "pointing_detect.h"
#include "pointer_reprobe.h"

static int failures = 0;
#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); \
            failures++;                                           \
        }                                                         \
    } while (0)

/* ---- mocks ---- */
static uint32_t                now_ms;
static sval_pointer_identity_t own; // what sval_pointer_identity reports
static uint8_t                 own_bus;
static uint8_t                 attached;     // module on the connector (a sensor code), or NO_RESPONSE
static uint8_t                 attached_bus; // the bus it answers on
static bool                    auto_build;   // sval_pointer_select starts it (pointing_auto.c)
static bool                    init_works       = true;
static uint8_t                 retry_own_result = SVAL_RETRY_NONE;
static int                     probe_calls, probe_calls_by_bus[8], retry_calls, select_calls, inits;
static uint32_t                search_times[64];
static int                     searches;
static uint32_t                last_activity_ms; // the last retry_own or bus probe
static bool                    any_activity;
static uint8_t                 last_bus_in_pass; // a pass probes at most one bus
static int                     buses_this_pass;

uint32_t timer_read32(void) {
    return now_ms;
}
uint32_t timer_elapsed32(uint32_t last) {
    return now_ms - last;
}
void sval_pointer_identity(sval_pointer_identity_t *id) {
    *id = own;
}
uint8_t sval_pointer_own_bus(void) {
    return own_bus;
}

uint8_t sval_pointer_retry_own(void) {
    retry_calls++;
    last_activity_ms = now_ms;
    any_activity     = true;
    if (searches < 64) search_times[searches] = now_ms;
    searches++;
    if (retry_own_result == SVAL_RETRY_OK) own.flags |= SVAL_POINTER_INIT_OK; // its driver started
    return retry_own_result;
}

static int bus_index(uint8_t bus) {
    return bus == SVAL_BUS_PS2 ? 0 : bus == SVAL_BUS_I2C ? 1 : 2;
}

sval_probe_result_t sval_probe_bus(uint8_t bus, bool patient) {
    (void)patient;
    probe_calls++;
    last_activity_ms = now_ms;
    any_activity     = true;
    probe_calls_by_bus[bus_index(bus)]++;
    buses_this_pass++;
    last_bus_in_pass      = bus;
    sval_probe_result_t r = {.detected = SVAL_SENSOR_NO_RESPONSE};
    if (attached != SVAL_SENSOR_NO_RESPONSE && attached_bus == bus) {
        r.detected = attached;
        r.id0      = 0x47;
        r.id1      = 0xB8;
    }
    return r;
}

void sval_pointer_select(const sval_probe_result_t *found) {
    select_calls++;
    if (!auto_build) return;
    inits++;
    own.built_for = found->detected;
    own.detected  = found->detected;
    own.flags     = SVAL_POINTER_AUTO | (init_works ? SVAL_POINTER_INIT_OK : 0);
}

static void reset(void) {
    now_ms = 0;
    memset(&own, 0, sizeof(own));
    own_bus          = 0;
    attached         = SVAL_SENSOR_NO_RESPONSE;
    attached_bus     = 0;
    auto_build       = false;
    init_works       = true;
    retry_own_result = SVAL_RETRY_NONE;
    probe_calls = retry_calls = select_calls = inits = searches = 0;
    last_activity_ms                                            = 0;
    any_activity                                                = false;
    memset(probe_calls_by_bus, 0, sizeof(probe_calls_by_bus));
}

// An auto build (pointing_auto.c) with nothing found yet: the trackpad's I2C is its own bus.
static void auto_half(void) {
    reset();
    auto_build = true;
    own.flags  = SVAL_POINTER_AUTO;
    own_bus    = SVAL_BUS_I2C;
}

// Runs the main loop (one task call per ms) until t_ms.
static void run_until(uint32_t t_ms) {
    while (now_ms < t_ms) {
        buses_this_pass = 0;
        sval_pointer_probe_task();
        CHECK(buses_this_pass <= 1);
        now_ms++;
    }
}

/* ---- tests ---- */

static void schedule(void) {
    CHECK(sval_reprobe_due_ms(0) == 1500);
    CHECK(sval_reprobe_due_ms(1) == 3000);
    CHECK(sval_reprobe_due_ms(2) == 6000);
    CHECK(sval_reprobe_due_ms(3) == 12000);
    CHECK(sval_reprobe_due_ms(4) == 30000);
    CHECK(sval_reprobe_due_ms(5) == 0); // no more
    CHECK(sval_reprobe_due_ms(1000) == 0);
    CHECK(sval_reprobe_due_ms(UINT16_MAX) == 0);
}

// Nothing attached: searches at exactly 1.5, 3, 6, 12 and 30 s, each probing PS/2 then SPI
// (I2C is the trackpad's, checked by retry_own), one bus per pass; nothing after 30 s.
static void nothing_attached_stops_at_30_s(void) {
    auto_half();
    run_until(1499);
    CHECK(probe_calls == 0);
    run_until(1510);
    CHECK(sval_pointer_searched());
    CHECK(retry_calls == 0); // the boot search follows the driver's own start
    CHECK(probe_calls == 2 && probe_calls_by_bus[0] == 1 && probe_calls_by_bus[1] == 0 && probe_calls_by_bus[2] == 1);
    CHECK(sval_pointer_found()->detected == SVAL_SENSOR_NO_RESPONSE);
    run_until(10UL * 60 * 1000); // ten minutes
    // retry_own runs at the start of every search after the first.
    CHECK(searches == 4);
    CHECK(search_times[0] == 3000 && search_times[1] == 6000 && search_times[2] == 12000 && search_times[3] == 30000);
    CHECK(probe_calls == 2 * 5);
    CHECK(last_activity_ms < 30010);
    CHECK(select_calls == 0);
}

// No probe after 30 s, ever: ten minutes, then across the 49.7-day clock wrap.
static void never_probes_after_30_s(void) {
    auto_half();
    run_until(30010);
    int probes = probe_calls, retries = retry_calls;
    run_until(10UL * 60 * 1000);
    now_ms = UINT32_MAX - 60000; // just before the wrap, then a minute past it
    for (uint32_t i = 0; i < 120000; i++) {
        sval_pointer_probe_task();
        now_ms++;
    }
    CHECK(probe_calls == probes && retry_calls == retries);
    CHECK(last_activity_ms < 30010);
}

// A module the search finds, plugged in at plug_ms: started at start_ms, or never (0).
static void plugged_in_at(uint32_t plug_ms, uint32_t start_ms) {
    auto_half();
    run_until(plug_ms);
    attached     = SVAL_SENSOR_PMW3389;
    attached_bus = SVAL_BUS_SPI;
    if (start_ms) {
        run_until(start_ms);
        CHECK(select_calls == 0);
        run_until(start_ms + 10);
        CHECK(select_calls == 1 && inits == 1);
        CHECK(own.flags & SVAL_POINTER_INIT_OK);
        CHECK(sval_pointer_found()->detected == SVAL_SENSOR_PMW3389);
        int probes = probe_calls, retries = retry_calls;
        run_until(30UL * 60 * 1000); // started once; nothing probed afterwards
        CHECK(probe_calls == probes && retry_calls == retries && select_calls == 1);
    } else {
        run_until(30UL * 60 * 1000);
        CHECK(select_calls == 0 && !(own.flags & SVAL_POINTER_INIT_OK));
        CHECK(sval_pointer_found()->detected == SVAL_SENSOR_NO_RESPONSE);
        CHECK(last_activity_ms < 30010);
    }
}
static void plugged_in_at_10_s_starts_at_12_s(void) {
    plugged_in_at(10000, 12000);
}
static void plugged_in_at_20_s_starts_at_30_s(void) {
    plugged_in_at(20000, 30000);
}
static void plugged_in_at_40_s_never_found(void) {
    plugged_in_at(40000, 0);
}

// The trackpad answers its own bus at 7 s: the 12 s search's retry_own starts it; the other
// buses aren't probed in that search, nor ever again.
static void own_module_retry_starts_it(void) {
    auto_half();
    run_until(7000);
    int probes       = probe_calls;
    retry_own_result = SVAL_RETRY_OK;
    run_until(12000 + 10);
    CHECK(retry_calls == 3);
    CHECK(probe_calls == probes);
    run_until(20UL * 60 * 1000);
    CHECK(retry_calls == 3 && probe_calls == probes);
}

// A half whose module works at boot is never searched: no retry_own, no probe, ever.
static void working_module_never_probed(void) {
    reset();
    own.built_for = SVAL_SENSOR_PMW3389;
    own.detected  = SVAL_SENSOR_PMW3389;
    own.flags     = SVAL_POINTER_INIT_OK;
    own_bus       = SVAL_BUS_SPI;
    run_until(60UL * 60 * 1000);
    CHECK(probe_calls == 0 && retry_calls == 0 && select_calls == 0);
    CHECK(sval_pointer_searched());
}

// Same for an auto build whose trackpad answered at boot.
static void working_auto_trackpad_never_probed(void) {
    reset();
    own.built_for = SVAL_SENSOR_AZOTEQ;
    own.detected  = SVAL_SENSOR_AZOTEQ;
    own.flags     = SVAL_POINTER_INIT_OK | SVAL_POINTER_AUTO;
    own_bus       = SVAL_BUS_PS2 | SVAL_BUS_I2C | SVAL_BUS_SPI;
    run_until(60UL * 60 * 1000);
    CHECK(probe_calls == 0 && retry_calls == 0);
}

// A module that answers but won't start is tried SVAL_REPROBE_INIT_TRIES times, then left.
static void failing_start_gives_up(void) {
    auto_half();
    init_works   = false;
    attached     = SVAL_SENSOR_PMW3389;
    attached_bus = SVAL_BUS_SPI;
    run_until(60UL * 60 * 1000);
    CHECK(inits == SVAL_REPROBE_INIT_TRIES);
}

// The same through retry_own (a per-module trackball build whose sensor answers).
static void failing_own_start_gives_up(void) {
    reset();
    own.built_for    = SVAL_SENSOR_PMW3389;
    own.detected     = SVAL_SENSOR_NO_RESPONSE;
    own_bus          = SVAL_BUS_SPI;
    retry_own_result = SVAL_RETRY_FAILED;
    run_until(60UL * 60 * 1000);
    CHECK(retry_calls == SVAL_REPROBE_INIT_TRIES);
    CHECK(probe_calls == 2); // only the boot search's PS/2 and I2C
}

// A per-module build that finds another module on another bus stops: it can't drive it, and
// 0x2C names it for the host. One that its own bus answers with the wrong sensor, too.
static void per_module_build_stops_on_other_module(void) {
    reset();
    own.built_for = SVAL_SENSOR_PMW3389;
    own.detected  = SVAL_SENSOR_NO_RESPONSE;
    own_bus       = SVAL_BUS_SPI;
    attached      = SVAL_SENSOR_TRACKPOINT;
    attached_bus  = SVAL_BUS_PS2;
    run_until(60UL * 60 * 1000);
    CHECK(probe_calls == 1 && select_calls == 1 && retry_calls == 0);
    CHECK(sval_pointer_found()->detected == SVAL_SENSOR_TRACKPOINT);
}

static void per_module_build_stops_on_wrong_sensor(void) {
    reset();
    own.built_for = SVAL_SENSOR_PMW3360;
    own.detected  = SVAL_SENSOR_PMW3389;
    own_bus       = SVAL_BUS_SPI;
    run_until(60UL * 60 * 1000);
    CHECK(probe_calls == 0 && retry_calls == 0);
}

// retry_own STOP (a TrackPoint build: its PS/2 driver can't start twice) ends the search.
static void retry_own_stop_ends_it(void) {
    reset();
    own.built_for    = SVAL_SENSOR_TRACKPOINT;
    own.detected     = SVAL_SENSOR_NO_RESPONSE;
    own_bus          = SVAL_BUS_PS2;
    retry_own_result = SVAL_RETRY_STOP;
    run_until(60UL * 60 * 1000);
    CHECK(retry_calls == 1);
    CHECK(probe_calls == 2); // the boot search's I2C and SPI only
}

// A module unplugged before a search finds it leaves the last find in place until a search
// ends: the report changes only when a search has finished.
static void found_only_changes_at_search_end(void) {
    auto_half();
    init_works   = false;
    attached     = SVAL_SENSOR_PMW3389;
    attached_bus = SVAL_BUS_SPI;
    run_until(1510);
    CHECK(sval_pointer_found()->detected == SVAL_SENSOR_PMW3389);
    attached = SVAL_SENSOR_NO_RESPONSE;
    run_until(3001); // the 3 s search has started (PS/2 pass) but not finished
    CHECK(sval_pointer_found()->detected == SVAL_SENSOR_PMW3389);
    run_until(3010);
    CHECK(sval_pointer_found()->detected == SVAL_SENSOR_NO_RESPONSE);
}

// A half that boots late (the half without USB: ~2 s for USB, then the link) is past the 1.5
// and 3 s due times at once. The second search still waits SVAL_REPROBE_GAP_MS after the
// first, so a trackball the first one reset (pointing_detect.c) has had its 50 ms.
static void late_boot_searches_spaced(void) {
    auto_half();
    now_ms = 3500;
    run_until(3503); // the first search: its start, then PS/2 and SPI, one per pass
    CHECK(probe_calls == 2 && retry_calls == 0);
    run_until(3502 + SVAL_REPROBE_GAP_MS);
    CHECK(retry_calls == 0);
    run_until(3600);
    CHECK(retry_calls == 1 && search_times[0] >= 3502 + SVAL_REPROBE_GAP_MS);
    run_until(10UL * 60 * 1000);
    CHECK(searches == 4); // 6, 12 and 30 s as before
    CHECK(search_times[1] == 6000 && search_times[2] == 12000 && search_times[3] == 30000);
}

// The gap never delays a search that's due on time.
static void gap_keeps_the_schedule(void) {
    CHECK(SVAL_REPROBE_GAP_MS < 1500);
}

typedef void (*test_fn)(void);
int main(void) {
    static const test_fn tests[] = {schedule, nothing_attached_stops_at_30_s, never_probes_after_30_s, plugged_in_at_10_s_starts_at_12_s, plugged_in_at_20_s_starts_at_30_s, plugged_in_at_40_s_never_found, own_module_retry_starts_it, working_module_never_probed, working_auto_trackpad_never_probed, failing_start_gives_up, failing_own_start_gives_up, per_module_build_stops_on_other_module, per_module_build_stops_on_wrong_sensor, retry_own_stop_ends_it, found_only_changes_at_search_end, late_boot_searches_spaced, gap_keeps_the_schedule};
    int                  failed  = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        fflush(stdout);
        pid_t pid = fork(); // pointer_reprobe.c keeps its state in statics: a fresh process each
        if (pid == 0) {
            tests[i]();
            fflush(stdout);
            _exit(failures != 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed++;
    }
    printf(failed ? "%d FAILED\n" : "OK\n", failed);
    return failed != 0;
}
