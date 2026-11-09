// Host test of the trackball driver's health check and recovery (trackball/trackball.c) against
// a simulated PMW3389: a healthy sensor is only read, a power-reset or stuck one is caught and
// restored, a half that never had a sensor is left to the search, and a sensor that is gone is
// given up on.
#include <stdio.h>
#include <stdlib.h>
#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/pmw33xx_common.h"
#include "svalboard.h"

#ifndef SIM_NONAUTO
#    define SVAL_POINTING_AUTO
#endif
#define SVAL_TRACKBALL_HOST_TEST
#define POINTING_DEVICE_IS_PMW3389

uint32_t sim_ms, sim_stall_us;
int      sim_status;

// The simulated chip.
enum { HEALTHY, STUCK, RESET, GONE, INITFAIL, BADID }; // INITFAIL: IDs fine, the start fails; BADID: IDs that are not a pair until reset
static struct {
    int      state;
    uint8_t  srom_id, config2, lift;
    uint16_t cpi;
    int      writes, writes_not_burst, inits;
    int      cs_pulses, resets;  // selects of the chip (a CS pulse) and 0x5A writes
    bool     drop_cpi;           // CPI writes are ignored
    int      glitch_raw;         // the next CPI register reads return 0
    bool     glitching;
} chip;
#define DEFAULT_CPI 1600

static void chip_start(void) { // what a start (pmw33xx_init) leaves
    chip.state   = HEALTHY;
    chip.srom_id = 0x04;
    chip.lift    = 0x00; // the start writes it; a reset leaves 0x02
    chip.config2 = 0x00;
    chip.cpi     = DEFAULT_CPI;
}
bool pmw33xx_init(uint8_t s) {
    (void)s;
    chip.inits++;
    if (chip.state == GONE || chip.state == INITFAIL || chip.state == BADID) return false;
    chip_start();
    return true;
}
bool pmw33xx_spi_start(uint8_t s) {
    (void)s;
    chip.cs_pulses++;
    return true;
}
static uint16_t cpi_raw(uint16_t cpi) { // as pmw3360.c and pmw3389.c write it
    uint16_t steps = CONSTRAIN(cpi / PMW33XX_CPI_STEP, PMW33XX_CPI_MIN / PMW33XX_CPI_STEP, PMW33XX_CPI_MAX / PMW33XX_CPI_STEP) - 1U;
#ifdef SIM_PMW3360
    return steps & 0xFF;
#else
    return steps;
#endif
}
uint8_t pmw33xx_read(uint8_t s, uint8_t reg) {
    (void)s;
    if (chip.state == GONE || chip.state == STUCK) return 0x00;
    if (chip.state == BADID) return reg == REG_Product_ID || reg == REG_Inverse_Product_ID ? 0x22 : 0x00;
    switch (reg) {
        case REG_Product_ID: return 0x47;
        case REG_Inverse_Product_ID: return 0xB8;
        case REG_Config2: return chip.config2;
#ifdef SIM_PMW3360
        case REG_Config1:
            if (chip.glitch_raw > 0) {
                chip.glitch_raw--;
                return 0;
            }
            return cpi_raw(chip.cpi);
#else
        case REG_Resolution_L:
            if (chip.glitch_raw > 0) {
                chip.glitch_raw--;
                chip.glitching = true;
                return 0;
            }
            return cpi_raw(chip.cpi) & 0xFF;
        case REG_Resolution_H:
            if (chip.glitching) {
                chip.glitching = false;
                return 0;
            }
            return cpi_raw(chip.cpi) >> 8;
#endif
        case REG_SROM_ID: return 0; // reads 0 on a healthy sensor too
        case REG_Lift_Config: return chip.lift;
    }
    return 0;
}
bool pmw33xx_write(uint8_t s, uint8_t reg, uint8_t v) {
    (void)s;
    chip.writes++;
    if (reg != REG_Motion_Burst) chip.writes_not_burst++;
    if (reg == REG_Power_Up_Reset && v == 0x5A) chip.resets++;
    if ((chip.state == STUCK || chip.state == BADID) && reg == REG_Power_Up_Reset && v == 0x5A) chip.state = HEALTHY; // a stuck sensor answers after the reset (its settings need a start)
    if (reg == REG_Config2 && chip.state == HEALTHY) chip.config2 = v;
    if (reg == REG_Lift_Config && chip.state == HEALTHY) chip.lift = v;
    return true;
}
uint16_t pmw33xx_get_cpi(uint8_t s) {
    (void)s;
#ifdef SIM_PMW3360
    chip.writes_not_burst++; // the PMW3360 driver writes Config1 back after reading it
#endif
    return chip.state == GONE || chip.state == STUCK ? 0 : chip.cpi;
}
void pmw33xx_set_cpi_all_sensors(uint16_t cpi) {
    if (chip.state == HEALTHY && !chip.drop_cpi) chip.cpi = cpi;
}
pmw33xx_report_t pmw33xx_read_burst(uint8_t s) {
    (void)s;
    pmw33xx_report_t r = {0};
    return r;
}
uint8_t pmw33xx_burst_extra[6];
bool    pmw33xx_burst_has_extra;
uint8_t sval_pmw33xx_classify(uint8_t p, uint8_t i) {
    if (p == 0x47 && i == 0xB8) return SVAL_SENSOR_PMW3389;
    if (p == 0x42 && i == 0xBD) return SVAL_SENSOR_PMW3360;
    return SVAL_SENSOR_NO_RESPONSE;
}

#include "../../keyboards/svalboard/trackball/trackball.c"

// squal.c: the quality stream (Sval 0x39), not under test here.
void sval_squal_set_capable(bool is_pmw3389) {
    (void)is_pmw3389;
}
void sval_squal_record(const sval_squal_read_t *q) {
    (void)q;
}

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #c);             \
            fails++;                                                \
        }                                                           \
    } while (0)

// Main loop passes of 1 ms for ms milliseconds.
static void run(uint32_t ms) {
    report_mouse_t r = {0};
    for (uint32_t i = 0; i < ms; i++) {
        sim_ms++;
        pointing_device_driver_get_report(r);
    }
}
// A cold start with the chip in state, before anything has run.
static void cold(int state) {
    memset(&chip, 0, sizeof(chip));
    sensor_init_ok = sensor_was_running = sensor_gave_up = sensor_saw_pair = false;
    sensor_probe_starts = 0;
    sensor_recover_fails = 0;
    sensor_recover_wait = sensor_recover_at = sensor_check_at = 0;
    sensor_product_id = sensor_inverse_product_id = 0;
    sim_ms = 1000; // uptime in ms, so the probes' 30 s waits count from boot
    sim_stall_us = 0;
    sim_status = POINTING_DEVICE_STATUS_UNKNOWN;
    global_saved_values.idle_flags = SVAL_IDLE_POINTER_REST;
    trackball_cached_cpi           = 0;
    chip_start();
    chip.state = state;
}
static void fresh(uint8_t idle_flags, uint16_t cpi) {
    memset(&chip, 0, sizeof(chip));
    sensor_init_ok = sensor_was_running = sensor_gave_up = sensor_saw_pair = false;
    sensor_probe_starts = 0;
    sensor_recover_fails = 0;
    sensor_recover_wait = sensor_recover_at = sensor_check_at = 0;
    sensor_product_id = sensor_inverse_product_id = 0;
    sim_status = POINTING_DEVICE_STATUS_SUCCESS;
    sim_ms = 100000;
    sim_stall_us = 0;
    global_saved_values.idle_flags = idle_flags;
    trackball_cached_cpi           = 0;
    chip_start();
    if (cpi) pointing_device_driver_set_cpi(cpi);
    CHECK(pointing_device_driver_init());
    if (cpi) pointing_device_driver_set_cpi(cpi);
    chip.inits = 0;
    chip.writes = chip.writes_not_burst = 0;
}

static void healthy_is_only_read(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    run(30000);
    CHECK(sensor_init_ok);
    CHECK(chip.inits == 0);
    CHECK(chip.writes_not_burst == 0); // no setting is ever written by the check
    CHECK(chip.config2 == 0x20);
}

static void power_reset_is_caught(void) {
    // Reset puts Config2 and the CPI at their defaults and drops the SROM.
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = RESET; // IDs still answer
    chip.config2 = 0x20; // reset value of Config2 is the rest-mode value
    chip.cpi     = DEFAULT_CPI;
    chip.lift    = 0x02;
    run(4500);
    CHECK(chip.inits == 1);
    CHECK(sensor_init_ok);
    CHECK(chip.state == HEALTHY && chip.lift == 0x00); // start settings back
    CHECK(chip.cpi == 800);                               // the asked CPI back
    CHECK(chip.config2 == 0x20);                          // rest mode back

    // Reset to the same Config2 and the default CPI as what we run: only Lift_Config tells.
    fresh(SVAL_IDLE_POINTER_REST, DEFAULT_CPI);
    chip.state   = RESET;
    chip.config2 = 0x20;
    chip.lift    = 0x02;
    run(4500);
    CHECK(chip.inits == 1);
    CHECK(chip.state == HEALTHY && chip.lift == 0x00);
}

static void stuck_is_caught(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = STUCK;
    run(4500);
    CHECK(chip.inits == 1 && sensor_init_ok && chip.state == HEALTHY);
}

static void never_started_is_left_to_the_search(void) {
    fresh(0, 0);
    sensor_init_ok = sensor_was_running = sensor_saw_pair = false; // this half never had a sensor
    chip.state = GONE;
    run(120000);
    CHECK(chip.inits == 0);
    CHECK(sim_stall_us == 0);
}

static void gone_is_given_up_on(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = GONE;
    sim_stall_us = 0;
    run(120000);
    CHECK(!sensor_init_ok);
    CHECK(chip.inits == SENSOR_RECOVER_TRIES);
    CHECK(sensor_gave_up);
    CHECK(sim_stall_us < 3000); // checks only; the simulated starts cost no time here
    int inits = chip.inits;
    run(120000);
    CHECK(chip.inits == inits); // no more tries
    // Keybard still sees the last good IDs while INIT_OK is clear.
    sval_pointer_identity_t id = {0};
    sval_pointer_identity(&id);
    CHECK(id.product_id == 0x47 && id.inverse_product_id == 0xB8 && !(id.flags & SVAL_POINTER_INIT_OK));
    // The status says it is down, with no mode.
    sval_idle_status_t st = {0};
    sval_pointer_status(&st);
    CHECK(st.sensor_present == 0 && st.sensor_mode == 0);
    // The sensor comes back with nothing else running (searches stopped after 30 s): the slow probe
    // finds it within 30 s and starts it, and the checks and the backoff begin again.
    chip.state = HEALTHY;
    run(31000);
    CHECK(sensor_init_ok && !sensor_gave_up && sensor_recover_fails == 0);
    CHECK(chip.inits == inits + 1);
    sval_pointer_status(&st);
    CHECK(st.sensor_present == 1);
    chip.state = STUCK; // and a loss after that is recovered again, from the first try
    run(4500);
    CHECK(sensor_init_ok && chip.state == HEALTHY);
}

static void gone_then_stuck_gets_reset_by_the_slow_probe(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = GONE;
    run(60000);
    CHECK(sensor_gave_up);
    chip.state = STUCK; // answers 0x00 until the power-up reset: the first probe sends it, the next starts it
    run(65000);
    CHECK(sensor_init_ok && !sensor_gave_up);
}


// What QMK does with the status (quantum/pointing_device/pointing_device.c): init sets it from the
// driver's start, then pointing_device_init_kb(); pointing_device_task() returns before it reads
// the other half's shared report unless the status is SUCCESS (POINTING_DEVICE_COMBINED).
static void qmk_init(void) {
    sim_status = pointing_device_driver_init() ? POINTING_DEVICE_STATUS_SUCCESS : POINTING_DEVICE_STATUS_INIT_FAILED;
    pointing_device_init_kb();
}
static int16_t qmk_task_x(int16_t sub_x, bool *ran) {
    *ran = pointing_device_get_status() == POINTING_DEVICE_STATUS_SUCCESS;
    if (!*ran) return 0;
    report_mouse_t own = pointing_device_driver_get_report((report_mouse_t){0});
    return own.x + sub_x;
}

static void master_boot_failure_keeps_the_sub_flowing(void) {
    cold(GONE); // the master's own sensor does not start
    qmk_init();
    bool ran;
    CHECK(sim_status == POINTING_DEVICE_STATUS_SUCCESS);
    CHECK(qmk_task_x(7, &ran) == 7 && ran); // the sub's report still reaches the host
    run(10000);
    CHECK(qmk_task_x(7, &ran) == 7 && ran);
    sval_pointer_identity_t id = {0};
    sval_pointer_identity(&id);
    CHECK(!(id.flags & SVAL_POINTER_INIT_OK)); // 0x2C still says the own sensor is down
    sval_idle_status_t st = {0};
    sval_pointer_status(&st);
    CHECK(st.sensor_present == 0);
    // What QMK alone does, for the record: INIT_FAILED blocks the sub.
    sim_status = POINTING_DEVICE_STATUS_INIT_FAILED;
    CHECK(qmk_task_x(7, &ran) == 0 && !ran);
}

static void boot_ids_that_are_not_a_pair_get_the_reset(void) {
    cold(BADID);
    CHECK(!pointing_device_driver_init());
    CHECK(chip.cs_pulses >= 1 && chip.resets == 1); // CS pulse and 0x5A
    CHECK(chip.state == HEALTHY);
}

static void slow_probe_resets_ids_that_are_not_a_pair(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = GONE;
    run(60000);
    CHECK(sensor_gave_up);
    chip.state = BADID; // answers 0x22 0x22, not 0x00: not a pair either
    int resets = chip.resets, pulses = chip.cs_pulses, inits = chip.inits;
    run(31000);
    CHECK(chip.resets == resets + 1 && chip.cs_pulses > pulses); // reset by the probe
    CHECK(chip.inits == inits);                                  // and not started on a non-pair
    run(31000);
    CHECK(sensor_init_ok && !sensor_gave_up); // the next probe reads a pair and starts it
}

static void sensor_that_appears_late_on_a_half_that_had_one(void) {
    cold(INITFAIL); // answers as a PMW3389 but the start fails
    CHECK(!pointing_device_driver_init());
    CHECK(sensor_saw_pair && !sensor_was_running);
    chip.inits = 0;
    run(28000);
    CHECK(chip.inits == 0); // the searches cover the first 30 s
    run(30000);             // to ~60 s: the probe looks every 30 s and the start still fails
    CHECK(!sensor_init_ok);
    CHECK(chip.inits <= 2);
    chip.state = HEALTHY; // the sensor's start works at ~60 s
    run(31000);
    CHECK(sensor_init_ok && !sensor_gave_up);
    // A half that never read a trackball is left to the searches.
    cold(GONE);
    CHECK(!pointing_device_driver_init());
    chip.inits = 0;
    chip.state = HEALTHY;
    run(120000);
    CHECK(chip.inits == 0 && !sensor_init_ok);
}

static void probe_starts_are_capped(void) {
    cold(INITFAIL);
    CHECK(!pointing_device_driver_init());
    chip.inits = 0;
    run(400000); // a sensor that answers but never starts
    CHECK(chip.inits == SENSOR_PROBE_START_TRIES);
    chip.state = HEALTHY; // the cap holds: no more starts, however long it waits
    run(100000);
    CHECK(chip.inits == SENSOR_PROBE_START_TRIES && !sensor_init_ok);
}

#ifdef SIM_NONAUTO
// The per-module build's search looks at the sensor itself (sval_pointer_retry_own).
static void search_retry_resets_ids_that_are_not_a_pair(void) {
    cold(BADID);
    CHECK(sval_pointer_retry_own() == SVAL_RETRY_NONE);
    CHECK(chip.cs_pulses >= 1 && chip.resets == 1 && !sensor_saw_pair);
    chip.state = HEALTHY; // reset: the next search reads a pair and starts it
    CHECK(sval_pointer_retry_own() == SVAL_RETRY_OK && sensor_init_ok && sensor_saw_pair);
}

static void search_and_probe_do_not_share_a_window(void) {
    cold(INITFAIL);
    run(29000); // no pair seen yet: nothing to probe
    CHECK(chip.inits == 0);
    CHECK(sval_pointer_retry_own() == SVAL_RETRY_FAILED); // the last search: a start, and the pair is noted
    CHECK(sensor_saw_pair && chip.inits == 1);
    run(29000); // the probe stays out for 30 s after the search
    CHECK(chip.inits == 1);
    run(2000);
    CHECK(chip.inits == 2); // then it looks
}
#endif

static void failed_cpi_write_causes_no_restart_loop(void) {
    // A read that glitches right after the write must not become the health check's reference.
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.glitch_raw = 1;
    pointing_device_driver_set_cpi(1600);
    CHECK(chip.cpi == 1600 && sensor_cpi_seen == cpi_raw(1600));
    run(30000);
    CHECK(chip.inits == 0 && sensor_init_ok);
    // A write the sensor ignores leaves the reference at what it holds.
    chip.drop_cpi = true;
    pointing_device_driver_set_cpi(2400);
    CHECK(sensor_cpi_seen == cpi_raw(1600) && trackball_cached_cpi == 2400);
    run(30000);
    CHECK(chip.inits == 0 && sensor_init_ok);
}

int main(void) {
    healthy_is_only_read();
    power_reset_is_caught();
    stuck_is_caught();
    never_started_is_left_to_the_search();
    gone_is_given_up_on();
    gone_then_stuck_gets_reset_by_the_slow_probe();
    master_boot_failure_keeps_the_sub_flowing();
    boot_ids_that_are_not_a_pair_get_the_reset();
    slow_probe_resets_ids_that_are_not_a_pair();
    sensor_that_appears_late_on_a_half_that_had_one();
    failed_cpi_write_causes_no_restart_loop();
    probe_starts_are_capped();
#ifdef SIM_NONAUTO
    search_retry_resets_ids_that_are_not_a_pair();
    search_and_probe_do_not_share_a_window();
#endif
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
