// Trackball (PMW3360/PMW3389) custom driver wrapper
// Calls upstream driver functions directly - no pointing_device.c patch needed

#include "pointing_detect.h"
#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/pmw33xx_common.h"
#include "svalboard.h"

static uint16_t trackball_cached_cpi = 0;

// The sensor's own rest modes: Run -> Rest1 (3 mA, 1 ms frames) after ~0.5 s without motion,
// Rest2 (100 ms frames) after ~10 s, Rest3 (500 ms frames) after ~10 min; motion wakes it on its
// own. QMK's init disables them (Config2 = 0), so we re-enable after init when the flag is set.
#define PMW33XX_CONFIG2_REST_EN 0x20
static uint8_t config2_shadow = 0x00;
static uint8_t last_motion    = 0;      // last Motion byte from the burst read
static bool    have_motion    = false;

void sval_pointer_rest_apply(void) {
    bool on = (global_saved_values.idle_flags & SVAL_IDLE_POINTER_REST) != 0;
    config2_shadow = on ? PMW33XX_CONFIG2_REST_EN : 0x00;
    pmw33xx_write(0, REG_Config2, config2_shadow);
}

void sval_pointer_status(sval_idle_status_t *st) {
    st->sensor_present = 1;
    st->sensor_config2 = config2_shadow;
    st->sensor_mode    = (last_motion >> 1) & 0x03;   // OP_MODE bits
    if (have_motion) st->sensor_mode |= 0x80;
    if (last_motion & 0x08) st->sensor_mode |= 0x40;  // Lift_stat
}

// The chip's own ID, read after init whether or not init succeeded. PMW3360 and
// PMW3389 need different SROM blobs, so firmware for one leaves the other dead;
// these let a host name the right .uf2 (Sval 0x2C).
static uint8_t sensor_product_id         = 0;
static uint8_t sensor_inverse_product_id = 0;
static bool    sensor_init_ok            = false;

// The SPI pins back on the SPI hardware. spi_init() sets them once, and the module search
// (pointing_detect.c) takes them as plain GPIO, so a start after a search sets them again.
static void trackball_spi_pins(void) {
    spi_init();
    spi_stop(); // a failed transfer can leave the bus started, and the next start would refuse it
#if defined(USE_GPIOV1) // as spi_init() sets them (platforms/chibios/drivers/spi_master.c)
    palSetPadMode(PAL_PORT(SPI_SCK_PIN), PAL_PAD(SPI_SCK_PIN), SPI_SCK_PAL_MODE);
    palSetPadMode(PAL_PORT(SPI_MOSI_PIN), PAL_PAD(SPI_MOSI_PIN), SPI_MOSI_PAL_MODE);
    palSetPadMode(PAL_PORT(SPI_MISO_PIN), PAL_PAD(SPI_MISO_PIN), SPI_MISO_PAL_MODE);
#else
    palSetPadMode(PAL_PORT(SPI_SCK_PIN), PAL_PAD(SPI_SCK_PIN), SPI_SCK_FLAGS);
    palSetPadMode(PAL_PORT(SPI_MOSI_PIN), PAL_PAD(SPI_MOSI_PIN), SPI_MOSI_FLAGS);
    palSetPadMode(PAL_PORT(SPI_MISO_PIN), PAL_PAD(SPI_MISO_PIN), SPI_MISO_FLAGS);
#endif
}

// Also starts it again after a failed start (svalboard.c's search, pointing_auto.c).
bool pointing_device_driver_init(void) {
    trackball_spi_pins();
    bool ok = pmw33xx_init(0);
    sensor_product_id         = pmw33xx_read(0, REG_Product_ID);
    sensor_inverse_product_id = pmw33xx_read(0, REG_Inverse_Product_ID);
    sensor_init_ok            = ok;
    if (ok) {
        sval_pointer_rest_apply();
        // The start puts the sensor at its default CPI; the one asked for before stays.
        if (trackball_cached_cpi) pmw33xx_set_cpi_all_sensors(trackball_cached_cpi);
    }
    return ok;
}

#ifndef SVAL_POINTING_AUTO
// A search's second look at the sensor of a per-module trackball build: when it now answers
// with its Product_ID (two register reads, ~0.4 ms), the driver starts again. The auto
// builds find a trackball with the search's own SPI probe instead (pointing_auto.c).
uint8_t sval_pointer_retry_own(void) {
    if (sensor_init_ok) return SVAL_RETRY_OK;
    trackball_spi_pins();
    uint8_t product = pmw33xx_read(0, REG_Product_ID), inverse = pmw33xx_read(0, REG_Inverse_Product_ID);
    uint8_t sensor  = sval_pmw33xx_classify(product, inverse);
    if (sensor == SVAL_SENSOR_NO_RESPONSE) {
        pmw33xx_write(0, REG_Power_Up_Reset, 0x5a); // a stuck sensor answers after this; the next search reads it
        return SVAL_RETRY_NONE;
    }
#    if defined(POINTING_DEVICE_IS_PMW3389)
    const uint8_t built_for = SVAL_SENSOR_PMW3389;
#    else
    const uint8_t built_for = SVAL_SENSOR_PMW3360;
#    endif
    if (sensor != built_for) { // the other sensor, which needs the other .uf2: report it
        sensor_product_id         = product;
        sensor_inverse_product_id = inverse;
        return SVAL_RETRY_STOP;
    }
    if (!pointing_device_driver_init()) return SVAL_RETRY_FAILED;
    pointing_device_set_status(POINTING_DEVICE_STATUS_SUCCESS); // QMK polls it from now on
    return SVAL_RETRY_OK;
}
#endif

void sval_pointer_identity(sval_pointer_identity_t *id) {
#if defined(POINTING_DEVICE_IS_PMW3389)
    id->built_for = SVAL_SENSOR_PMW3389;
#else
    id->built_for = SVAL_SENSOR_PMW3360;
#endif
    id->product_id         = sensor_product_id;
    id->inverse_product_id = sensor_inverse_product_id;
    id->detected           = sval_pmw33xx_classify(sensor_product_id, sensor_inverse_product_id);
    if (sensor_init_ok) id->flags |= SVAL_POINTER_INIT_OK;
}

uint8_t sval_pointer_own_bus(void) {
    return SVAL_BUS_SPI;
}

// A running sensor is looked at about once a second: its Product_ID and Inverse_Product_ID are
// what the start read, and its Config2 is what we wrote (a power reset puts it back to 0).
// A sensor that glitched with the half's power (stuck, reset or shut down) answers differently,
// or not at all, and the start's cached answers would keep reporting "ok". Two bad looks in a
// row clear the flag (Sval 0x2C and 0x18 then say so) and restart the driver: its reset, SROM
// upload, CPI and rest mode (about 0.2 s). A start that fails is tried again after 1, 2, 4, 8,
// 16, then every 30 s, so a sensor that is really gone costs little. Once it starts, the half
// reports the sensor again.
#define SENSOR_CHECK_MS 1000
#define SENSOR_RECOVER_FIRST_MS 1000
#define SENSOR_RECOVER_MAX_MS 30000
static uint32_t sensor_check_at     = 0;
static uint32_t sensor_recover_at   = 0;
static uint16_t sensor_recover_wait = 0;

static bool sensor_answers(void) {
    uint8_t product = pmw33xx_read(0, REG_Product_ID), inverse = pmw33xx_read(0, REG_Inverse_Product_ID);
    uint8_t config2 = pmw33xx_read(0, REG_Config2);
    pmw33xx_write(0, REG_Config2, config2_shadow); // any write leaves burst mode; this one changes nothing
    return product == sensor_product_id && inverse == sensor_inverse_product_id && config2 == config2_shadow;
}

// True when the sensor is running and can be read.
static bool sensor_running(void) {
    uint32_t now = timer_read32();
    if (sensor_init_ok) {
        if (timer_elapsed32(sensor_check_at) < SENSOR_CHECK_MS) return true;
        sensor_check_at = now;
        if (sensor_answers()) return true;
        wait_us(500);
        if (sensor_answers()) return true;
        sensor_init_ok      = false;
        sensor_recover_wait = 0;
    }
    if (timer_elapsed32(sensor_recover_at) < sensor_recover_wait) return false;
    sensor_recover_at = now;
    if (pointing_device_driver_init()) { // sets sensor_init_ok, CPI and rest mode again
        sensor_recover_wait = 0;
        sensor_check_at     = timer_read32();
        return true;
    }
    sensor_recover_wait = sensor_recover_wait ? MIN(sensor_recover_wait * 2, SENSOR_RECOVER_MAX_MS) : SENSOR_RECOVER_FIRST_MS;
    return false;
}

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    if (!sensor_running()) return mouse_report;
    // Same as pmw33xx_get_report, but keep the Motion byte so the idle diagnostics can
    // report which run/rest mode the sensor is actually in.
    pmw33xx_report_t report = pmw33xx_read_burst(0);
    last_motion = report.motion.w;
    have_motion = true;
    if (report.motion.b.is_lifted || !report.motion.b.is_motion) {
        return mouse_report;
    }
    mouse_report.x = CONSTRAIN_HID_XY(report.delta_x);
    mouse_report.y = CONSTRAIN_HID_XY(report.delta_y);

    // Axis swap for svalboard orientation
    int16_t swap = mouse_report.x;
    mouse_report.x = -mouse_report.y;
    mouse_report.y = -swap;

    return mouse_report;
}

uint16_t pointing_device_driver_get_cpi(void) {
    return trackball_cached_cpi;
}

void pointing_device_driver_set_cpi(uint16_t cpi) {
    if (cpi != trackball_cached_cpi) {
        pmw33xx_set_cpi_all_sensors(cpi);
        trackball_cached_cpi = cpi;
    }
}
