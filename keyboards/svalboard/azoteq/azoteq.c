// Azoteq IQS5xx custom driver wrapper
// Calls upstream driver functions directly - no pointing_device.c patch needed

#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/azoteq_iqs5xx.h"
#include "svalboard.h"
#include "pointing_detect.h"
#include "pad_gestures.h"

#ifdef SVAL_AZOTEQ_MULTITOUCH
// Native touchpad builds (azoteq/multitouch.mk): every contact is read, and
// the reads are timed to the chip's window (pad_schedule.h).
#    include "multitouch.h"
#    include "pad_schedule.h"
#    ifdef SVAL_PERF_STATS
#        include "../perf.h"
#    endif
_Static_assert(CH_CFG_ST_FREQUENCY == 1000000, "the pad schedule assumes a 1 MHz system timer");
// The driver's last contact read: status, the read's own start and end, and
// the contact slots it carried.
i2c_status_t       azoteq_iqs5xx_last_read(uint32_t *start_us, uint32_t *end_us, bool *retried, uint8_t *slots);
static pad_sched_t azo_sched;
static bool        azo_sched_ready = false;

uint32_t sval_mt_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

#    ifdef SVAL_PERF_STATS
void sval_pad_sched_stats(uint32_t *lead_us, uint32_t *xfer_us, uint32_t *wait_us) {
    *lead_us = azo_sched_ready ? (uint32_t)azo_sched.lead : 0;
    *xfer_us = azo_sched_ready && azo_sched.xfer[0] != UINT32_MAX ? azo_sched.xfer[0] : 0; // no contacts
    *wait_us = azo_sched_ready ? azo_sched.wait : 0;
}
#    endif

// The contact path keeps the chip at its full resolution (2048 x 1792), so
// the pointer DPI is applied here, to the mouse output, exactly as the chip
// would at the resolution the DPI asks for (drivers/sensors/azoteq_iqs5xx.c,
// azoteq_iqs5xx_set_cpi). Remainders carry, so slow motion is not lost.
#    define AZO_FULL_X 2048u
#    define AZO_FULL_Y 1792u
#    define AZO_RES(cpi, mm, full) ((((uint32_t)(cpi) * (mm) * 10u + 127u) / 254u) < (full) ? (((uint32_t)(cpi) * (mm) * 10u + 127u) / 254u) : (full))
static uint32_t azo_num_x = AZO_FULL_X, azo_num_y = AZO_FULL_Y;
static int32_t  azo_rem[4];

static int32_t azo_scale(int32_t v, uint32_t num, uint32_t den, int32_t *rem) {
    if (num >= den) return v;
    int32_t t = v * (int32_t)num + *rem;
    int32_t q = t / (int32_t)den;
    *rem      = t - q * (int32_t)den;
    return q;
}
#else
#    ifdef SVAL_PERF_STATS
#        include "perf.h"
#    endif
// Azoteq can only report at 100Hz. Faster polling causes issues.
#    define AZO_MS 10

static uint16_t azo_timer = 0;
#endif
static uint8_t azo_held_buttons = 0;
static uint16_t azo_cached_cpi = 0;

// Public in the driver but missing from its header.
i2c_status_t azoteq_iqs5xx_end_session(void);

static uint16_t azo_product = AZOTEQ_IQS5XX_UNKNOWN;

// An IQS5xx in a low-power mode NACKs the first addressing, then holds SCL after the next until its
// communication window opens (datasheet 8.8.2), which can take longer than the driver's 10 ms
// timeout. Opening a window first lets what follows run inside it: an I2C STOP doesn't close it.
#define AZO_I2C_ADDRESS (0x74 << 1)
#define AZO_WAKE_TIMEOUT_MS 300
static void azo_wake(void) {
    i2c_init();
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (i2c_ping_address(AZO_I2C_ADDRESS, AZO_WAKE_TIMEOUT_MS) == I2C_STATUS_SUCCESS) return;
        wait_us(200); // the datasheet asks for at least 150 us before addressing again
    }
}

// ------------------------------------------------------------------------
// TPS43 on 0.7 mm glass (docs/tps43-glass.md).
//
// The module's stored settings assume a 1 mm plastic overlay. The thinner
// glass gives about twice the touch deltas, so the stock touch thresholds
// fire on noise and drift. Azoteq applications engineering's settings for
// 0.7 mm glass (2026-04-24) double the touch multipliers and let the part
// fall into its low-power, environment-tracking mode sooner. They are written
// at run time after every start of the chip, because the start resets the
// chip (its runtime registers return to the stored settings) and the driver
// then sets the idle timeout to 255 s. Nothing here writes the chip's stored
// settings image.
#define SVAL_IQS_REG_LP1_REPORT_RATE 0x0580   // 2 bytes, ms
#define SVAL_IQS_REG_IDLE_MODE_TIMEOUT 0x0586 // 1 byte, s
#define SVAL_IQS_REG_TOUCH_MULT_SET 0x0596    // set + clear, 1 byte each
#define SVAL_IQS_REG_EXPORT_VERSION 0x0677    // 2 bytes: the stored settings' export version
#define SVAL_IQS_REG_IDENTITY 0x0000          // product(2) project(2) major(1) minor(1)

// Azoteq-recommended values; override in config.h to experiment.
#ifndef SVAL_IQS_TOUCH_MULT_SET
#    define SVAL_IQS_TOUCH_MULT_SET 32 // stock ~16
#endif
#ifndef SVAL_IQS_TOUCH_MULT_CLEAR
#    define SVAL_IQS_TOUCH_MULT_CLEAR 24 // stock ~12
#endif
#ifndef SVAL_IQS_LP1_REPORT_RATE_MS
#    define SVAL_IQS_LP1_REPORT_RATE_MS 50 // stock 80
#endif
// Seconds of no touch before the part drops from Idle into LP1 (ALP mode).
// Azoteq: 2 s. The stock QMK driver sets 255 (never) to avoid the LP1 report
// rate adding up to SVAL_IQS_LP1_REPORT_RATE_MS of first-touch latency; set
// this to 255 to get that behavior back if the wake-up feels laggy.
#ifndef SVAL_IQS_IDLE_MODE_TIMEOUT_S
#    define SVAL_IQS_IDLE_MODE_TIMEOUT_S 2
#endif

static i2c_status_t sval_iqs_write(uint16_t reg, const uint8_t *data, uint16_t len) {
    return i2c_write_register16(AZO_I2C_ADDRESS, reg, data, len, AZOTEQ_IQS5XX_TIMEOUT_MS);
}

// The touchpad tap setting (pad_gestures.h): tap, two-finger tap, press and
// hold and the hold time, over what the driver's start wrote from the build's
// defines. Written at every start of the chip (with the glass tuning) and on
// every change. A setting of 0 is the build's defaults, which the start has
// just written, so a board that never changed it writes nothing more.
static volatile bool azo_gestures_pending = false;

// Inside an open communication window; leaves it open.
static i2c_status_t azo_write_gestures(void) {
    uint8_t      regs[SVAL_PAD_REG_BYTES];
    i2c_status_t status = i2c_read_register16(AZO_I2C_ADDRESS, SVAL_PAD_REG_GESTURES, regs, sizeof(regs), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status != I2C_STATUS_SUCCESS) return status;
    sval_pad_regs_apply(sval_pad_gestures(), regs);
    return sval_iqs_write(SVAL_PAD_REG_GESTURES, regs, sizeof(regs));
}

bool sval_pad_gestures_supported(void) {
    return true;
}

void sval_pad_gestures_changed(void) {
    azo_gestures_pending = true;
}

// Inside an open communication window; ends it.
static i2c_status_t sval_iqs_apply_tuning(void) {
    i2c_status_t status = I2C_STATUS_SUCCESS;

    if (sval_pad_gestures() != 0 || azo_gestures_pending) status |= azo_write_gestures();

    const uint8_t mult[2] = {SVAL_IQS_TOUCH_MULT_SET, SVAL_IQS_TOUCH_MULT_CLEAR};
    status |= sval_iqs_write(SVAL_IQS_REG_TOUCH_MULT_SET, mult, sizeof(mult));

    const uint8_t lp1[2] = {(SVAL_IQS_LP1_REPORT_RATE_MS >> 8) & 0xFF, SVAL_IQS_LP1_REPORT_RATE_MS & 0xFF};
    status |= sval_iqs_write(SVAL_IQS_REG_LP1_REPORT_RATE, lp1, sizeof(lp1));

    const uint8_t idle_tmo = SVAL_IQS_IDLE_MODE_TIMEOUT_S;
    status |= sval_iqs_write(SVAL_IQS_REG_IDLE_MODE_TIMEOUT, &idle_tmo, 1);

    azoteq_iqs5xx_end_session();
    return status;
}

static void sval_iqs_tune(void) {
    i2c_status_t status = sval_iqs_apply_tuning();
    if (status != I2C_STATUS_SUCCESS) {
        azo_wake();
        status = sval_iqs_apply_tuning();
    }
    if (status == I2C_STATUS_SUCCESS) azo_gestures_pending = false;
    dprintf("IQS5XX: glass tuning %s (i2c status %d)\n", status == I2C_STATUS_SUCCESS ? "applied" : "FAILED", status);
}

// The pad's identity, read once and logged once on the console (a few
// seconds after boot, so a console opened at plug-in sees it). The settings
// export version tells whether a pad's stored settings were ever rewritten.
static struct {
    uint16_t product, project, export_version;
    uint8_t  major, minor;
    bool     read, logged;
} azo_id;

#define AZO_ID_LOG_AFTER_MS 5000

// Inside an open communication window; leaves it open.
static void azo_read_identity(void) {
    if (azo_id.read) return;
    uint8_t buf[6] = {0}, ver[2] = {0};
    if (i2c_read_register16(AZO_I2C_ADDRESS, SVAL_IQS_REG_IDENTITY, buf, sizeof(buf), AZOTEQ_IQS5XX_TIMEOUT_MS) != I2C_STATUS_SUCCESS) return;
    if (i2c_read_register16(AZO_I2C_ADDRESS, SVAL_IQS_REG_EXPORT_VERSION, ver, sizeof(ver), AZOTEQ_IQS5XX_TIMEOUT_MS) != I2C_STATUS_SUCCESS) return;
    azo_id.product        = ((uint16_t)buf[0] << 8) | buf[1];
    azo_id.project        = ((uint16_t)buf[2] << 8) | buf[3];
    azo_id.major          = buf[4];
    azo_id.minor          = buf[5];
    azo_id.export_version = ((uint16_t)ver[0] << 8) | ver[1];
    azo_id.read           = true;
}

static void azo_log_identity(void) {
    if (!azo_id.read || azo_id.logged || timer_read32() < AZO_ID_LOG_AFTER_MS) return;
    azo_id.logged = true;
    uprintf("IQS5XX %s pad: product %u project %u version %u.%u settings export version %u\n", is_keyboard_left() ? "left" : "right", azo_id.product, azo_id.project, azo_id.major, azo_id.minor, azo_id.export_version);
}

bool pointing_device_driver_init(void) {
    azo_wake();
    azoteq_iqs5xx_init();
    // The product number, read again here because init's result doesn't say whether one answered.
    azo_wake();
    azo_product = azoteq_iqs5xx_get_product();
    if (azo_product != AZOTEQ_IQS5XX_UNKNOWN) {
        azo_read_identity();
        sval_iqs_tune(); // ends the session
    }
    return true;
}

// The chip's full start again (azoteq/multitouch.c, after the chip reported a
// reset), with the glass tuning after it.
void sval_azoteq_restart(void) {
    azoteq_iqs5xx_init();
    azo_wake();
    if (azoteq_iqs5xx_get_product() != AZOTEQ_IQS5XX_UNKNOWN) sval_iqs_tune();
}

void sval_pointer_identity(sval_pointer_identity_t *id) {
    id->built_for = SVAL_SENSOR_AZOTEQ;
    if (azo_product != AZOTEQ_IQS5XX_UNKNOWN) {
        id->detected           = SVAL_SENSOR_AZOTEQ;
        id->product_id         = azo_product >> 8;
        id->inverse_product_id = azo_product & 0xFF;
        id->flags |= SVAL_POINTER_INIT_OK;
    } else {
        id->detected = SVAL_SENSOR_NO_RESPONSE;
    }
}

uint8_t sval_pointer_own_bus(void) {
    return SVAL_BUS_I2C;
}

#ifndef SVAL_POINTING_AUTO
// A search's second look at the pad of a trackpad build: one addressing, and the full start
// only when it answers (the auto builds have their own, pointing_auto.c).
uint8_t sval_pointer_retry_own(void) {
    if (azo_product != AZOTEQ_IQS5XX_UNKNOWN) return SVAL_RETRY_OK;
    if (!gpio_read_pin(I2C1_SDA_PIN) || !gpio_read_pin(I2C1_SCL_PIN)) return SVAL_RETRY_NONE; // no pull-ups: nothing there
    i2c_init();
    i2c_status_t st = i2c_ping_address(AZO_I2C_ADDRESS, 5);
    if (st != I2C_STATUS_SUCCESS) {
        wait_us(200);
        st = i2c_ping_address(AZO_I2C_ADDRESS, 5);
    }
    if (st == I2C_STATUS_ERROR) return SVAL_RETRY_NONE;
    pointing_device_driver_init();
    if (azo_product == AZOTEQ_IQS5XX_UNKNOWN) return SVAL_RETRY_FAILED;
    pointing_device_set_status(POINTING_DEVICE_STATUS_SUCCESS);
    if (azo_cached_cpi) { // the start reset the chip's resolution
        uint16_t cpi   = azo_cached_cpi;
        azo_cached_cpi = 0;
        pointing_device_driver_set_cpi(cpi);
    }
    return SVAL_RETRY_OK;
}
#endif

// A changed tap setting, written in a window of its own.
static void azo_gestures_task(void) {
    if (!azo_gestures_pending || azo_product == AZOTEQ_IQS5XX_UNKNOWN) return;
    azo_wake();
    i2c_status_t status = azo_write_gestures();
    azoteq_iqs5xx_end_session();
    if (status != I2C_STATUS_SUCCESS) {
        azo_wake();
        status = azo_write_gestures();
        azoteq_iqs5xx_end_session();
    }
    azo_gestures_pending = false; // a failure is not retried here; the next start writes it
    dprintf("IQS5XX: tap setting %02X %s\n", sval_pad_gestures(), status == I2C_STATUS_SUCCESS ? "written" : "FAILED");
}

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    azo_log_identity();
    azo_gestures_task();
#ifdef SVAL_AZOTEQ_MULTITOUCH
    if (!azo_sched_ready) {
        pad_sched_init(&azo_sched);
        azo_sched_ready = true;
    }
    if (pad_sched_due(&azo_sched, sval_mt_now_us())) {
#    ifdef SVAL_PERF_STATS
        uint32_t call_started = sval_mt_now_us();
        uint8_t  perf_prev    = sval_perf_enter(SVAL_PERF_GAP_PAD);
#    endif
        mouse_report     = azoteq_iqs5xx_get_report(mouse_report);
#    ifdef SVAL_PERF_STATS
        sval_perf_leave(perf_prev);
        sval_perf_section(SVAL_PERF_SEC_PAD_CALL, sval_mt_now_us() - call_started);
#    endif
        azo_held_buttons = mouse_report.buttons;
        uint32_t start, end;
        bool     retried;
        uint8_t  slots;
        bool     ok = azoteq_iqs5xx_last_read(&start, &end, &retried, &slots) == I2C_STATUS_SUCCESS;
        pad_sched_done(&azo_sched, start, end, ok, !retried, slots, AZOTEQ_IQS5XX_REPORT_RATE * 1000u);
        mouse_report.x = azo_scale(mouse_report.x, azo_num_x, AZO_FULL_X, &azo_rem[0]);
        mouse_report.y = azo_scale(mouse_report.y, azo_num_y, AZO_FULL_Y, &azo_rem[1]);
        mouse_report.h = azo_scale(mouse_report.h, azo_num_x, AZO_FULL_X, &azo_rem[2]);
        mouse_report.v = azo_scale(mouse_report.v, azo_num_y, AZO_FULL_Y, &azo_rem[3]);
    }
#else
    if (timer_elapsed(azo_timer) >= AZO_MS) {
        azo_timer = timer_read();
#    ifdef SVAL_PERF_STATS
        // Timed around the whole call: the read and its end of session.
        uint32_t started   = sval_perf_now_us();
        uint8_t  perf_prev = sval_perf_enter(SVAL_PERF_GAP_PAD);
#    endif
        mouse_report = azoteq_iqs5xx_get_report(mouse_report);
#    ifdef SVAL_PERF_STATS
        sval_perf_leave(perf_prev);
        sval_perf_i2c(sval_perf_now_us() - started, true);
#    endif
        azo_held_buttons = mouse_report.buttons;
    }
#endif

    // Invert V-axis
    mouse_report.v = -mouse_report.v;
    mouse_report.buttons = azo_held_buttons;

#ifdef SVAL_AZOTEQ_MULTITOUCH
    // The native touchpad has this pad: its mouse output stays zero.
    if (sval_mt_suppress_hand(is_keyboard_left() ? 0 : 1)) {
        mouse_report     = (report_mouse_t){0};
        azo_held_buttons = 0;
    }
#    ifdef MOUSE_EXTENDED_REPORT
    // The half without USB marks its split pointing report with a counter
    // that moves only when its pad has news (multitouch.c), so the USB half
    // fetches the pad snapshot only then. It is 0 unless the USB half asked
    // for it; boot_x is otherwise always zero here (host_mouse_send fills
    // it), and the USB half clears it on arrival (svalboard.c).
    if (!is_keyboard_master()) mouse_report.boot_x = (int8_t)sval_mt_split_hint();
#    endif
#endif
    return mouse_report;
}

uint16_t pointing_device_driver_get_cpi(void) {
    return azo_cached_cpi;
}

void pointing_device_driver_set_cpi(uint16_t cpi) {
    if (cpi != azo_cached_cpi) {
        azoteq_iqs5xx_set_cpi(cpi);
        azo_cached_cpi = cpi;
#ifdef SVAL_AZOTEQ_MULTITOUCH
        azo_num_x = cpi ? AZO_RES(cpi, 43u, AZO_FULL_X) : AZO_FULL_X;
        azo_num_y = cpi ? AZO_RES(cpi, 40u, AZO_FULL_Y) : AZO_FULL_Y;
        memset(azo_rem, 0, sizeof(azo_rem));
#endif
    }
}
