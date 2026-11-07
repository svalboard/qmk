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

#define SVAL_POINTING_AUTO
#define POINTING_DEVICE_IS_PMW3389

uint32_t sim_ms, sim_stall_us;

// The simulated chip.
enum { HEALTHY, STUCK, RESET, GONE };
static struct {
    int      state;
    uint8_t  srom_id, config2, lift;
    uint16_t cpi;
    int      writes, writes_not_burst, inits;
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
    if (chip.state == GONE) return false;
    chip_start();
    return true;
}
uint8_t pmw33xx_read(uint8_t s, uint8_t reg) {
    (void)s;
    if (chip.state == GONE || chip.state == STUCK) return 0x00;
    switch (reg) {
        case REG_Product_ID: return 0x47;
        case REG_Inverse_Product_ID: return 0xB8;
        case REG_Config2: return chip.config2;
        case REG_SROM_ID: return 0; // reads 0 on a healthy sensor too
        case REG_Lift_Config: return chip.lift;
    }
    return 0;
}
bool pmw33xx_write(uint8_t s, uint8_t reg, uint8_t v) {
    (void)s;
    chip.writes++;
    if (reg != REG_Motion_Burst) chip.writes_not_burst++;
    if (reg == REG_Config2 && chip.state == HEALTHY) chip.config2 = v;
    if (reg == REG_Lift_Config && chip.state == HEALTHY) chip.lift = v;
    return true;
}
uint16_t pmw33xx_get_cpi(uint8_t s) {
    (void)s;
    return chip.state == GONE || chip.state == STUCK ? 0 : chip.cpi;
}
void pmw33xx_set_cpi_all_sensors(uint16_t cpi) {
    if (chip.state == HEALTHY) chip.cpi = cpi;
}
pmw33xx_report_t pmw33xx_read_burst(uint8_t s) {
    (void)s;
    pmw33xx_report_t r = {0};
    return r;
}
uint8_t sval_pmw33xx_classify(uint8_t p, uint8_t i) {
    if (p == 0x47 && i == 0xB8) return SVAL_SENSOR_PMW3389;
    if (p == 0x42 && i == 0xBD) return SVAL_SENSOR_PMW3360;
    return SVAL_SENSOR_NO_RESPONSE;
}

#include "../../keyboards/svalboard/trackball/trackball.c"

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
static void fresh(uint8_t idle_flags, uint16_t cpi) {
    memset(&chip, 0, sizeof(chip));
    sensor_init_ok = sensor_was_running = sensor_gave_up = false;
    sensor_recover_fails = 0;
    sensor_recover_wait = sensor_recover_at = sensor_check_at = 0;
    sensor_product_id = sensor_inverse_product_id = 0;
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
    run(2500);
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
    run(2500);
    CHECK(chip.inits == 1);
    CHECK(chip.state == HEALTHY && chip.lift == 0x00);
}

static void stuck_is_caught(void) {
    fresh(SVAL_IDLE_POINTER_REST, 800);
    chip.state = STUCK;
    run(2500);
    CHECK(chip.inits == 1 && sensor_init_ok && chip.state == HEALTHY);
}

static void never_started_is_left_to_the_search(void) {
    fresh(0, 0);
    sensor_init_ok = sensor_was_running = false; // this half never had a sensor
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
    // The sensor comes back and a later search starts the driver: checks and recovery resume.
    chip.state = HEALTHY;
    CHECK(pointing_device_driver_init());
    CHECK(!sensor_gave_up && sensor_recover_fails == 0);
    chip.state = STUCK;
    run(2500);
    CHECK(sensor_init_ok && chip.state == HEALTHY);
}

int main(void) {
    healthy_is_only_read();
    power_reset_is_caught();
    stuck_is_caught();
    never_started_is_left_to_the_search();
    gone_is_given_up_on();
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
