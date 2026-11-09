// Trackball (PMW3360/PMW3389) custom driver wrapper
// Calls upstream driver functions directly - no pointing_device.c patch needed

#include "pointing_detect.h"
#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/pmw33xx_common.h"
#include "svalboard.h"
#include "squal.h"

static uint16_t trackball_cached_cpi = 0;

// The CPI register(s) read directly. pmw33xx_get_cpi() on the PMW3360 writes Config1 back, so
// the health check never uses it. PMW3360: Config1; PMW3389: Resolution_L and Resolution_H.
static uint16_t sensor_cpi_raw(void) {
#ifdef REG_Config1
    return pmw33xx_read(0, REG_Config1);
#else
    return pmw33xx_read(0, REG_Resolution_L) | (uint16_t)(pmw33xx_read(0, REG_Resolution_H) << 8);
#endif
}
static uint8_t  sensor_recover_fails = 0; // restarts that failed since the sensor was last running
static bool     sensor_gave_up       = false;

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

static bool sensor_init_ok = false; // the driver is running (cleared while the health check has found it down)

void sval_pointer_status(sval_idle_status_t *st) {
    st->sensor_present = sensor_init_ok; // not present while it is down, and then no mode either
    st->sensor_config2 = config2_shadow;
    st->sensor_mode    = 0;
    if (!sensor_init_ok) return;
    st->sensor_mode = (last_motion >> 1) & 0x03;   // OP_MODE bits
    if (have_motion) st->sensor_mode |= 0x80;
    if (last_motion & 0x08) st->sensor_mode |= 0x40;  // Lift_stat
}

// The chip's own ID, read after init whether or not init succeeded. PMW3360 and
// PMW3389 need different SROM blobs, so firmware for one leaves the other dead;
// these let a host name the right .uf2 (Sval 0x2C).
static uint8_t sensor_product_id         = 0;
static uint8_t sensor_inverse_product_id = 0;
// What a start leaves in the sensor, for the health check (below).
static uint16_t sensor_cpi_seen = 0;     // its CPI register(s), raw, after the start or the last CPI change
static uint8_t  sensor_lift_seen = 0;    // Lift_Config after the start (the start sets it; a power reset puts it back to 0x02)
static bool     sensor_was_running = false; // a start has succeeded in this session: only then is a loss recovered here
static bool     sensor_saw_pair    = false; // this session read this build's Product_ID pair, started or not (RAM only, never saved)

bool pmw33xx_spi_start(uint8_t sensor); // pmw33xx_common.c; selecting the chip pulses its CS line

#if defined(POINTING_DEVICE_IS_PMW3389)
#    define SVAL_BUILT_FOR SVAL_SENSOR_PMW3389
#else
#    define SVAL_BUILT_FOR SVAL_SENSOR_PMW3360
#endif

// A sensor that answers IDs which are not a pair is stuck. The CS pulse resets its serial
// port and 0x5A its core, as the search does (pointing_detect.c); the next look reads it after
// its 50 ms. Nothing here waits.
static void sensor_unstick(void) {
    if (pmw33xx_spi_start(0)) {
        wait_us(10);
        spi_stop();
    }
    wait_us(10);
    pmw33xx_write(0, REG_Power_Up_Reset, 0x5a);
}

// Full starts the slow probe may make for a sensor that never started (as #80's reprobe cap).
#define SENSOR_PROBE_START_TRIES 4
static uint8_t  sensor_probe_starts = 0;
static uint32_t sensor_recover_at   = 0;

// Notes that this build's sensor answered as itself, for the slow probe below. A search or a
// start that sees it holds the probe off for 30 s from now, so a reset is never followed by an
// ID read within the sensor's 50 ms, and a search's start and the probe's never land together.
static void sensor_saw(void) {
    sensor_saw_pair = true;
    if (!sensor_was_running) sensor_recover_at = timer_read32();
}
static void sensor_note_ids(uint8_t product, uint8_t inverse) {
    if (sval_pmw33xx_classify(product, inverse) == SVAL_BUILT_FOR) sensor_saw();
}

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
    uint8_t product = pmw33xx_read(0, REG_Product_ID), inverse = pmw33xx_read(0, REG_Inverse_Product_ID);
    // A restart of a sensor that was running keeps the last good IDs while it answers nothing,
    // so a host doesn't see "no trackball" for a moment (the INIT_OK flag reports the loss).
    uint8_t sensor = sval_pmw33xx_classify(product, inverse);
    if (!sensor_was_running || sensor != SVAL_SENSOR_NO_RESPONSE) {
        sensor_product_id         = product;
        sensor_inverse_product_id = inverse;
    }
    sensor_note_ids(product, inverse);
    if (!ok && sensor == SVAL_SENSOR_NO_RESPONSE) sensor_unstick(); // not a pair: reset it for the next look
    sensor_init_ok = ok;
    if (ok) {
#if defined(POINTING_DEVICE_IS_PMW3389)
        sval_squal_set_capable(true);
#else
        sval_squal_set_capable(false);
#endif
        sval_pointer_rest_apply();
        // The start puts the sensor at its default CPI; the one asked for before stays.
        if (trackball_cached_cpi) pmw33xx_set_cpi_all_sensors(trackball_cached_cpi);
        sensor_cpi_seen    = sensor_cpi_raw();
        sensor_lift_seen   = pmw33xx_read(0, REG_Lift_Config);
        sensor_was_running = true;
        sensor_recover_fails = 0;
        sensor_gave_up       = false;
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
        sensor_unstick(); // a stuck sensor answers after this; the next search reads it
        return SVAL_RETRY_NONE;
    }
    const uint8_t built_for = SVAL_BUILT_FOR;
    if (sensor == built_for) sensor_saw();
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

// A running sensor is looked at about every 3 s, reads only (no writes, so the rest-mode
// step-down is never disturbed): Product_ID and Inverse_Product_ID as the start read them,
// Config2 as the start wrote it, the CPI it reported after the start, and Lift_Config as the
// start left it (SROM_ID reads 0 on a healthy sensor here, so it can't tell). A sensor that glitched with the half's power answers differently or not
// at all: stuck (0x00 or 0xFF), or reset (IDs fine, but no SROM, and Config2, CPI and Lift_Config at
// their defaults, which only a start sets), and the start's cached answers would keep reporting "ok". Two bad looks
// in a row clear the flag (Sval 0x2C and 0x18 then say so) and restart the driver: its reset,
// SROM upload, CPI and rest mode (about 0.2 s). A restart that fails is tried again after
// 1, 2, 4 and 8 s; after SENSOR_RECOVER_TRIES failed restarts it stops restarting and only
// reads Product_ID and Inverse_Product_ID once every 30 s (about 0.4 ms; a stuck sensor also
// gets the power-up reset). When they answer as before, a full start runs, and on success
// the checks and the backoff begin again. A sensor that is really gone costs about 1 s of
// stall in total, then 0.4 ms every 30 s. Only a half whose sensor has started before recovers here: a half
// that never had a sensor leaves that to the search.
#define SENSOR_CHECK_MS 3000
#define SENSOR_RECOVER_FIRST_MS 1000
#define SENSOR_RECOVER_TRIES 5
#define SENSOR_SLOW_PROBE_MS 30000
static uint32_t sensor_check_at     = 0;
static uint16_t sensor_recover_wait = 0;

static bool sensor_answers(void) {
    uint8_t product = pmw33xx_read(0, REG_Product_ID), inverse = pmw33xx_read(0, REG_Inverse_Product_ID);
    bool    ok      = sval_pmw33xx_classify(product, inverse) != SVAL_SENSOR_NO_RESPONSE && product == sensor_product_id && inverse == sensor_inverse_product_id &&
              pmw33xx_read(0, REG_Config2) == config2_shadow && sensor_cpi_raw() == sensor_cpi_seen &&
              pmw33xx_read(0, REG_Lift_Config) == sensor_lift_seen;
    pmw33xx_write(0, REG_Motion_Burst, 0x00); // burst mode again, as the read path enters it; no setting changes
    return ok;
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
        dprintf("PMW33XX: failed its health check, restarting\n");
    }
    if (!sensor_was_running) {
        // A half that read this build's IDs but never got the sensor running (its start failed):
        // the searches cover the first 30 s, then this probe keeps looking. A half that never
        // saw a trackball leaves it to the searches.
        if (!sensor_saw_pair || sensor_probe_starts >= SENSOR_PROBE_START_TRIES) return false;
        sensor_gave_up = true;
    }
    if (sensor_gave_up) {
        if (timer_elapsed32(sensor_recover_at) < SENSOR_SLOW_PROBE_MS) return false;
        sensor_recover_at = now;
        trackball_spi_pins();
        uint8_t sensor = sval_pmw33xx_classify(pmw33xx_read(0, REG_Product_ID), pmw33xx_read(0, REG_Inverse_Product_ID));
        if (sensor == SVAL_SENSOR_NO_RESPONSE) {
            sensor_unstick(); // not a pair: a stuck sensor answers after this; the next probe reads it
        } else if (sensor == SVAL_BUILT_FOR) {
            if (!sensor_was_running) sensor_probe_starts++;
            if (pointing_device_driver_init()) { // clears sensor_gave_up and the failed-restart count
                sensor_recover_wait = 0;
                sensor_check_at     = timer_read32();
                return true;
            }
        }
        return false;
    }
    if (timer_elapsed32(sensor_recover_at) < sensor_recover_wait) return false;
    sensor_recover_at = now;
    if (pointing_device_driver_init()) { // sets sensor_init_ok, CPI and rest mode again
        sensor_recover_wait = 0;
        sensor_check_at     = timer_read32();
        return true;
    }
    if (++sensor_recover_fails >= SENSOR_RECOVER_TRIES) {
        sensor_gave_up = true;
        dprintf("PMW33XX: giving up restarts after %u, probing every 30 s\n", sensor_recover_fails);
        return false;
    }
    sensor_recover_wait = sensor_recover_wait ? sensor_recover_wait * 2 : SENSOR_RECOVER_FIRST_MS;
    return false;
}

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    if (!sensor_running()) return mouse_report;
    // Same as pmw33xx_get_report, but keep the Motion byte so the idle diagnostics can
    // report which run/rest mode the sensor is actually in.
    pmw33xx_report_t report = pmw33xx_read_burst(0);
    last_motion = report.motion.w;
    have_motion = true;
    if (pmw33xx_burst_has_extra) { // a host is polling Sval 0x39 (squal.c)
        sval_squal_read_t q = {
            .motion  = report.motion.w,
            .squal   = pmw33xx_burst_extra[0],
            .rawsum  = pmw33xx_burst_extra[1],
            .rawmax  = pmw33xx_burst_extra[2],
            .rawmin  = pmw33xx_burst_extra[3],
            .shutter = (uint16_t)((pmw33xx_burst_extra[4] << 8) | pmw33xx_burst_extra[5]),
        };
        sval_squal_record(&q);
    }
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

// What the CPI register(s) hold for a CPI, as the sensor's set_cpi writes it (pmw3360.c, pmw3389.c).
static uint16_t sensor_cpi_expected(uint16_t cpi) {
    uint16_t steps = CONSTRAIN(cpi / PMW33XX_CPI_STEP, PMW33XX_CPI_MIN / PMW33XX_CPI_STEP, PMW33XX_CPI_MAX / PMW33XX_CPI_STEP) - 1U;
#ifdef REG_Config1
    return steps & 0xFF;
#else
    return steps;
#endif
}

// The write returns nothing, so the register is read back. The health check's reference changes
// only when it holds the CPI asked for (one more write if it doesn't): a write that did not take
// leaves the reference at what the sensor holds, and the check finds nothing to restart for.
// The asked CPI stays cached either way, so a start applies it, and this runs once per change.
void pointing_device_driver_set_cpi(uint16_t cpi) {
    if (cpi == trackball_cached_cpi) return;
    pmw33xx_set_cpi_all_sensors(cpi);
    trackball_cached_cpi = cpi;
    if (!sensor_init_ok) return;
    uint16_t want = sensor_cpi_expected(cpi);
    uint16_t raw  = sensor_cpi_raw();
    if (raw != want) {
        pmw33xx_set_cpi_all_sensors(cpi);
        raw = sensor_cpi_raw();
    }
    if (raw == want) {
        sensor_cpi_seen = raw;
    } else {
        dprintf("PMW33XX: CPI write did not take (%u)\n", cpi);
    }
}

#if !defined(SVAL_POINTING_AUTO) || defined(SVAL_TRACKBALL_HOST_TEST)
// A sensor that fails to start sets QMK's status to INIT_FAILED, and with POINTING_DEVICE_COMBINED
// pointing_device_task() then returns before it reads the other half's shared report: the sub's
// ball would be dead too. The status stays SUCCESS instead; the driver returns an empty report
// while its own sensor is down (sensor_running), and Sval 0x2C still reports INIT_OK clear.
// The auto builds' init always succeeds, so they never need this.
void pointing_device_init_kb(void) {
    if (pointing_device_get_status() != POINTING_DEVICE_STATUS_SUCCESS) pointing_device_set_status(POINTING_DEVICE_STATUS_SUCCESS);
}
#endif
