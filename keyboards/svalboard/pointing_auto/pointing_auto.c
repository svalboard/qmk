// One firmware for every pointing module. At boot the trackball is looked for first (most
// boards have one): a PMW3360 or PMW3389 that answers starts right away. Then the trackpad,
// with its own driver over the I2C hardware, as the trackpad-only build does; the
// bit-banged I2C probe missed it. Otherwise nothing runs until svalboard.c's search
// (pointing_detect.h, first ~1.5 s after boot, then up to 30 s while nothing is found:
// pointer_reprobe.h) says what's attached; then that module's driver starts and handles
// everything. Changing modules takes a reset.
#include "quantum.h"
#include "pointing_device.h"
#include "pointing_detect.h"
#include "pointing_auto.h"
#include "drivers/sensors/azoteq_iqs5xx.h"

static const sval_pointer_driver_t *driver      = NULL; // running
static const sval_pointer_driver_t *failed      = NULL; // answered the search, didn't start; its identity is reported
static uint16_t                     pending_cpi = 0;

void sval_pointer_select(const sval_probe_result_t *found) {
    static const sval_pointer_driver_t *const drivers[] = {&sval_driver_pmw3360, &sval_driver_pmw3389, &sval_driver_azoteq, &sval_driver_trackpoint};
    if (driver) return;
    for (uint8_t i = 0; i < ARRAY_SIZE(drivers); i++) {
        if (drivers[i]->sensor != found->detected) continue;
        // The PS/2 driver claims a PIO state machine and program each time it starts, so a
        // TrackPoint is started once, whatever its start returned.
        if (drivers[i]->init() || drivers[i]->sensor == SVAL_SENSOR_TRACKPOINT) {
            driver = drivers[i];
            failed = NULL;
            if (pending_cpi) driver->set_cpi(pending_cpi);
        } else {
            failed = drivers[i]; // a later search that finds it again starts it again
        }
        return;
    }
}

// A search's second look at the trackpad, the one module this build checks on its own bus
// (the I2C hardware), before the other buses. A bare connector costs a line check and two
// addressings; only a chip that answers gets the full start (~110 ms, as at boot).
#define AUTO_AZO_ADDRESS (0x74 << 1)
#define AUTO_AZO_PING_MS 5
uint8_t sval_pointer_retry_own(void) {
    if (driver) return driver->sensor == SVAL_SENSOR_TRACKPOINT ? SVAL_RETRY_STOP : SVAL_RETRY_OK;
    // An I2C bus with nothing on it may have no pull-ups; a low line would hold the
    // hardware to its timeout, so there's nothing to address.
    if (!gpio_read_pin(I2C1_SDA_PIN) || !gpio_read_pin(I2C1_SCL_PIN)) return SVAL_RETRY_NONE;
    // A chip in a low-power mode NACKs the first addressing and wakes (azoteq/azoteq.c).
    i2c_status_t st = i2c_ping_address(AUTO_AZO_ADDRESS, AUTO_AZO_PING_MS);
    if (st != I2C_STATUS_SUCCESS) {
        wait_us(200);
        st = i2c_ping_address(AUTO_AZO_ADDRESS, AUTO_AZO_PING_MS);
    }
    if (st == I2C_STATUS_ERROR) return SVAL_RETRY_NONE; // no ACK: no trackpad
    sval_pointer_identity_t id = {0};
    sval_driver_azoteq.init();
    sval_driver_azoteq.identity(&id);
    if (id.detected != SVAL_SENSOR_AZOTEQ) return SVAL_RETRY_FAILED;
    driver = &sval_driver_azoteq;
    failed = NULL;
    if (pending_cpi) driver->set_cpi(pending_cpi);
    return SVAL_RETRY_OK;
}

bool pointing_device_driver_init(void) {
    // The trackball first. A silent sensor gets the power-up reset (pointing_detect.c), and
    // the 1.5 s search finds it.
    sval_probe_result_t ball = sval_probe_bus(SVAL_BUS_SPI, true);
    if (ball.detected == SVAL_SENSOR_PMW3360 || ball.detected == SVAL_SENSOR_PMW3389) {
        sval_pointer_select(&ball);
        if (driver) return true;
    }
    sval_pointer_identity_t id = {0};
    sval_driver_azoteq.init();
    sval_driver_azoteq.identity(&id);
    if (id.detected == SVAL_SENSOR_AZOTEQ) driver = &sval_driver_azoteq;
    return true; // any other module starts once the probe has found it
}

// The I2C pins belong to the hardware driver from boot, and the trackpad check has run there.
uint8_t sval_pointer_own_bus(void) {
    return driver ? SVAL_BUS_PS2 | SVAL_BUS_I2C | SVAL_BUS_SPI : SVAL_BUS_I2C;
}

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    return driver ? driver->get_report(mouse_report) : mouse_report;
}

uint16_t pointing_device_driver_get_cpi(void) {
    return driver ? driver->get_cpi() : pending_cpi;
}

void pointing_device_driver_set_cpi(uint16_t cpi) {
    pending_cpi = cpi;
    if (driver) driver->set_cpi(cpi);
}

void sval_pointer_identity(sval_pointer_identity_t *id) {
    if (driver) driver->identity(id);
    else if (failed) failed->identity(id);
    id->flags |= SVAL_POINTER_AUTO;
}

void sval_pointer_rest_apply(void) {
    if (driver && driver->rest_apply) driver->rest_apply();
}

void sval_pointer_status(sval_idle_status_t *st) {
    if (driver && driver->status) driver->status(st);
    else st->sensor_present = 0;
}

void recalibrate_pointer(void) {
    if (driver && driver->recalibrate) driver->recalibrate();
}
