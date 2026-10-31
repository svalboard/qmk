// Copyright 2023 Dasky (@daskygit)
// Copyright 2023 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "azoteq_iqs5xx.h"
#include "pointing_device_internal.h"
#include "wait.h"
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    include <string.h>
#    include "timer.h"
#endif

#ifndef AZOTEQ_IQS5XX_ADDRESS
#    define AZOTEQ_IQS5XX_ADDRESS (0x74 << 1)
#endif
#ifndef AZOTEQ_IQS5XX_TIMEOUT_MS
#    define AZOTEQ_IQS5XX_TIMEOUT_MS 10
#endif
#ifdef SVAL_AZOTEQ_MULTITOUCH
// Base data (cycle time to relative Y) before the contact slots.
#    define AZOTEQ_IQS5XX_BASE_BYTES 10
#    ifndef AZOTEQ_IQS5XX_CONTACT_TIMEOUT_MS
// Multiple contacts can keep the sensor computing past the legacy 10 ms
// deadline. Include clock stretching while awaiting its communication window.
#        define AZOTEQ_IQS5XX_CONTACT_TIMEOUT_MS 30
#    endif
#endif

#define AZOTEQ_IQS5XX_REG_PRODUCT_NUMBER 0x0000
#define AZOTEQ_IQS5XX_REG_PREVIOUS_CYCLE_TIME 0x000C
#define AZOTEQ_IQS5XX_REG_SYSTEM_CONTROL_1 0x0432
#define AZOTEQ_IQS5XX_REG_REPORT_RATE_ACTIVE 0x057A
#define AZOTEQ_IQS5XX_REG_IDLE_MODE_TIMEOUT 0x0586
#define AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_0 0x058E
#define AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_1 0x058F
#define AZOTEQ_IQS5XX_REG_X_RESOLUTION 0x066E
#define AZOTEQ_IQS5XX_REG_XY_CONFIG_0 0x0669
#define AZOTEQ_IQS5XX_REG_Y_RESOLUTION 0x0670
#define AZOTEQ_IQS5XX_REG_SINGLE_FINGER_GESTURES 0x06B7
#define AZOTEQ_IQS5XX_REG_END_COMMS 0xEEEE

// Gesture configuration
#ifndef AZOTEQ_IQS5XX_TAP_ENABLE
#    define AZOTEQ_IQS5XX_TAP_ENABLE true
#endif
#ifndef AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE
#    define AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE false
#endif
#ifndef AZOTEQ_IQS5XX_TWO_FINGER_TAP_ENABLE
#    define AZOTEQ_IQS5XX_TWO_FINGER_TAP_ENABLE true
#endif
#ifndef AZOTEQ_IQS5XX_SCROLL_ENABLE
#    define AZOTEQ_IQS5XX_SCROLL_ENABLE true
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_X_ENABLE
#    define AZOTEQ_IQS5XX_SWIPE_X_ENABLE false
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_Y_ENABLE
#    define AZOTEQ_IQS5XX_SWIPE_Y_ENABLE false
#endif
#ifndef AZOTEQ_IQS5XX_ZOOM_ENABLE
#    define AZOTEQ_IQS5XX_ZOOM_ENABLE false
#endif
#ifndef AZOTEQ_IQS5XX_TAP_TIME
#    define AZOTEQ_IQS5XX_TAP_TIME 0x96
#endif
#ifndef AZOTEQ_IQS5XX_TAP_DISTANCE
#    define AZOTEQ_IQS5XX_TAP_DISTANCE 0x19
#endif
#ifndef AZOTEQ_IQS5XX_HOLD_TIME
#    define AZOTEQ_IQS5XX_HOLD_TIME 0x12C
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_INITIAL_TIME
#    define AZOTEQ_IQS5XX_SWIPE_INITIAL_TIME 0x64 // 0x96
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_INITIAL_DISTANCE
#    define AZOTEQ_IQS5XX_SWIPE_INITIAL_DISTANCE 0x12C
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_TIME
#    define AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_TIME 0x0
#endif
#ifndef AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_DISTANCE
#    define AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_DISTANCE 0x7D0
#endif
#ifndef AZOTEQ_IQS5XX_SCROLL_INITIAL_DISTANCE
#    define AZOTEQ_IQS5XX_SCROLL_INITIAL_DISTANCE 0x32
#endif
#ifndef AZOTEQ_IQS5XX_ZOOM_INITIAL_DISTANCE
#    define AZOTEQ_IQS5XX_ZOOM_INITIAL_DISTANCE 0x32
#endif
#ifndef AZOTEQ_IQS5XX_ZOOM_CONSECUTIVE_DISTANCE
#    define AZOTEQ_IQS5XX_ZOOM_CONSECUTIVE_DISTANCE 0x19
#endif
#ifndef AZOTEQ_IQS5XX_EVENT_MODE
// Event mode can't be used until the pointing code has changed (stuck buttons)
#    define AZOTEQ_IQS5XX_EVENT_MODE false
#endif

#if defined(AZOTEQ_IQS5XX_TPS43)
#    define AZOTEQ_IQS5XX_WIDTH_MM 43
#    define AZOTEQ_IQS5XX_HEIGHT_MM 40
#    define AZOTEQ_IQS5XX_RESOLUTION_X 2048
#    define AZOTEQ_IQS5XX_RESOLUTION_Y 1792
#elif defined(AZOTEQ_IQS5XX_TPS65)
#    define AZOTEQ_IQS5XX_WIDTH_MM 65
#    define AZOTEQ_IQS5XX_HEIGHT_MM 49
#    define AZOTEQ_IQS5XX_RESOLUTION_X 3072
#    define AZOTEQ_IQS5XX_RESOLUTION_Y 2048
#elif !defined(AZOTEQ_IQS5XX_WIDTH_MM) && !defined(AZOTEQ_IQS5XX_HEIGHT_MM)
#    error "You must define one of the available azoteq trackpads or specify at least the width and height"
#endif

#define DIVIDE_UNSIGNED_ROUND(numerator, denominator) (((numerator) + ((denominator) / 2)) / (denominator))
#define AZOTEQ_IQS5XX_INCH_TO_RESOLUTION_X(inch) (DIVIDE_UNSIGNED_ROUND((inch) * (uint32_t)AZOTEQ_IQS5XX_WIDTH_MM * 10, 254))
#define AZOTEQ_IQS5XX_RESOLUTION_X_TO_INCH(px) (DIVIDE_UNSIGNED_ROUND((px) * (uint32_t)254, AZOTEQ_IQS5XX_WIDTH_MM * 10))
#define AZOTEQ_IQS5XX_INCH_TO_RESOLUTION_Y(inch) (DIVIDE_UNSIGNED_ROUND((inch) * (uint32_t)AZOTEQ_IQS5XX_HEIGHT_MM * 10, 254))
#define AZOTEQ_IQS5XX_RESOLUTION_Y_TO_INCH(px) (DIVIDE_UNSIGNED_ROUND((px) * (uint32_t)254, AZOTEQ_IQS5XX_HEIGHT_MM * 10))

const pointing_device_driver_t azoteq_iqs5xx_pointing_device_driver = {
    .init       = azoteq_iqs5xx_init,
    .get_report = azoteq_iqs5xx_get_report,
    .set_cpi    = azoteq_iqs5xx_set_cpi,
    .get_cpi    = azoteq_iqs5xx_get_cpi,
};

static uint16_t azoteq_iqs5xx_product_number = AZOTEQ_IQS5XX_UNKNOWN;

static struct {
    uint16_t resolution_x;
    uint16_t resolution_y;
} azoteq_iqs5xx_device_resolution_t;

i2c_status_t azoteq_iqs5xx_end_session(void) {
    const uint8_t END_BYTE = 1; // any data
    return i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_END_COMMS, &END_BYTE, 1, AZOTEQ_IQS5XX_TIMEOUT_MS);
}

#ifdef SVAL_AZOTEQ_MULTITOUCH
// The Svalboard native touchpad (keyboards/svalboard/azoteq/multitouch.c)
// reads every contact in the same window as the mouse data.
extern void     sval_mt_sample(const uint8_t *data, bool valid);
extern void     sval_mt_read_status(i2c_status_t read_status, i2c_status_t end_status, uint32_t elapsed_ms);
extern uint32_t sval_mt_i2c_error_flags(void);
extern void     sval_mt_acquisition_fault(uint32_t flags, bool retried, bool recovered);
extern uint32_t sval_mt_now_us(void);
#    ifdef SVAL_PERF_STATS
extern void sval_perf_i2c(uint32_t us, bool ok);
#    endif

#    define AZOTEQ_IQS5XX_SLOTS 5
#    define AZOTEQ_IQS5XX_SLOT_BYTES 7
#    define AZOTEQ_IQS5XX_RAW_BYTES (AZOTEQ_IQS5XX_BASE_BYTES + AZOTEQ_IQS5XX_SLOTS * AZOTEQ_IQS5XX_SLOT_BYTES)

// The last read, for the read schedule in the keyboard's wrapper
// (keyboards/svalboard/azoteq/azoteq.c): its status, when the first read
// itself (not a follow-up, the end of session, nor the processing after it)
// started and ended, and how many contact slots it carried, since its length
// depends on them. A retried read is not timed to the chip's window.
static i2c_status_t azoteq_iqs5xx_base_status = I2C_STATUS_SUCCESS;
static uint32_t     azoteq_iqs5xx_read_start_us, azoteq_iqs5xx_read_end_us;
static bool         azoteq_iqs5xx_read_retried;
static uint8_t      azoteq_iqs5xx_read_slots;
// Contact slots read with the base bytes: up to the highest slot that held a
// contact in the last good read. A contact keeps its slot until it lifts, so
// a lift can leave a contact in a higher slot than the count.
static uint8_t azoteq_iqs5xx_slots_next;

i2c_status_t azoteq_iqs5xx_last_read(uint32_t *start_us, uint32_t *end_us, bool *retried, uint8_t *slots) {
    *start_us = azoteq_iqs5xx_read_start_us;
    *end_us   = azoteq_iqs5xx_read_end_us;
    *retried  = azoteq_iqs5xx_read_retried;
    *slots    = azoteq_iqs5xx_read_slots;
    return azoteq_iqs5xx_base_status;
}

static bool azoteq_iqs5xx_slot_down(const uint8_t *raw, uint8_t slot) {
    const uint8_t *p = raw + AZOTEQ_IQS5XX_BASE_BYTES + AZOTEQ_IQS5XX_SLOT_BYTES * slot;
    return p[4] || p[5]; // touch strength, as multitouch.c reads it
}
#endif

i2c_status_t azoteq_iqs5xx_get_base_data(azoteq_iqs5xx_base_data_t *base_data) {
#ifdef SVAL_AZOTEQ_MULTITOUCH
    // One communication window for both the mouse data and every contact:
    // one read of the 10 base bytes and the 7-byte contact slots the last
    // read needed, so the read grows with the contacts down, not with the
    // five slots. Only when the count (raw[5]) says more contacts are down
    // than that read found does a second read fetch the remaining slots. The
    // window stays open until the end of session, so it sees the same cycle.
    uint8_t        raw[AZOTEQ_IQS5XX_RAW_BYTES] = {0};
    const uint8_t  first                        = azoteq_iqs5xx_slots_next;
    const uint16_t first_len                    = AZOTEQ_IQS5XX_BASE_BYTES + AZOTEQ_IQS5XX_SLOT_BYTES * first;
    uint32_t       started                      = timer_read32();

    azoteq_iqs5xx_read_retried  = false;
    azoteq_iqs5xx_read_slots    = first;
    azoteq_iqs5xx_read_start_us = sval_mt_now_us();
    i2c_status_t status         = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_PREVIOUS_CYCLE_TIME, raw, first_len, AZOTEQ_IQS5XX_CONTACT_TIMEOUT_MS);
    azoteq_iqs5xx_read_end_us   = sval_mt_now_us();
    // IQS5xx datasheet 8.8.2: a low-power wake may NACK the first address.
    // Retry exactly once after >=150 us, only for a NACK: the RP2040 driver
    // reports one as a failure with no error bits. Never retry a timeout, an
    // arbitration loss or overrun, or an end-session write.
    if (status != I2C_STATUS_SUCCESS) {
        uint32_t flags = sval_mt_i2c_error_flags();
        bool     retry = status == I2C_STATUS_ERROR && flags == 0;
        if (retry) {
            wait_us(200);
            memset(raw, 0, sizeof(raw));
            azoteq_iqs5xx_read_retried  = true;
            azoteq_iqs5xx_read_start_us = sval_mt_now_us();
            status                      = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_PREVIOUS_CYCLE_TIME, raw, first_len, AZOTEQ_IQS5XX_CONTACT_TIMEOUT_MS);
            azoteq_iqs5xx_read_end_us   = sval_mt_now_us();
        }
        sval_mt_acquisition_fault(flags, retry, status == I2C_STATUS_SUCCESS);
    }
    if (status == I2C_STATUS_SUCCESS) {
        // raw[5] is the number of contacts down.
        uint8_t found = 0, highest = 0;
        for (uint8_t i = 0; i < first; i++) {
            if (azoteq_iqs5xx_slot_down(raw, i)) {
                found++;
                highest = i + 1;
            }
        }
        if (raw[5] > found && first < AZOTEQ_IQS5XX_SLOTS) {
            const uint16_t offset = AZOTEQ_IQS5XX_BASE_BYTES + AZOTEQ_IQS5XX_SLOT_BYTES * first;
            status                = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_PREVIOUS_CYCLE_TIME + offset, raw + offset, sizeof(raw) - offset, AZOTEQ_IQS5XX_CONTACT_TIMEOUT_MS);
            for (uint8_t i = first; i < AZOTEQ_IQS5XX_SLOTS; i++)
                if (azoteq_iqs5xx_slot_down(raw, i)) highest = i + 1;
        }
        if (!raw[5]) {
            // All up: no slot carries a contact, whatever the chip left there.
            memset(raw + AZOTEQ_IQS5XX_BASE_BYTES, 0, sizeof(raw) - AZOTEQ_IQS5XX_BASE_BYTES);
            highest = 0;
        }
        if (status == I2C_STATUS_SUCCESS) azoteq_iqs5xx_slots_next = highest;
    }
#    ifdef SVAL_PERF_STATS
    sval_perf_i2c(sval_mt_now_us() - azoteq_iqs5xx_read_start_us, status == I2C_STATUS_SUCCESS);
#    endif
    i2c_status_t read_status = status, end_status = I2C_STATUS_SUCCESS;
    if (status == I2C_STATUS_SUCCESS) {
        memcpy(base_data, raw, sizeof(*base_data));
        status = end_status = azoteq_iqs5xx_end_session();
    }
    sval_mt_read_status(read_status, end_status, timer_elapsed32(started));
    azoteq_iqs5xx_base_status = status;
    sval_mt_sample(raw, status == I2C_STATUS_SUCCESS);
    return status;
#else
    i2c_status_t status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_PREVIOUS_CYCLE_TIME, (uint8_t *)base_data, 10, AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        azoteq_iqs5xx_end_session();
    }
    return status;
#endif
}

i2c_status_t azoteq_iqs5xx_get_report_rate(azoteq_iqs5xx_report_rate_t *report_rate, azoteq_iqs5xx_charging_modes_t mode, bool end_session) {
    if (mode > AZOTEQ_IQS5XX_LP2) {
        pd_dprintf("IQS5XX - Invalid mode for get report rate.\n");
        return I2C_STATUS_ERROR;
    }
    uint16_t     selected_reg = AZOTEQ_IQS5XX_REG_REPORT_RATE_ACTIVE + (2 * mode);
    i2c_status_t status       = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, selected_reg, (uint8_t *)report_rate, 2, AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_set_report_rate(uint16_t report_rate_ms, azoteq_iqs5xx_charging_modes_t mode, bool end_session) {
    if (mode > AZOTEQ_IQS5XX_LP2) {
        pd_dprintf("IQS5XX - Invalid mode for set report rate.\n");
        return I2C_STATUS_ERROR;
    }
    uint16_t                    selected_reg = AZOTEQ_IQS5XX_REG_REPORT_RATE_ACTIVE + (2 * mode);
    azoteq_iqs5xx_report_rate_t report_rate  = {0};
    report_rate.h                            = (uint8_t)((report_rate_ms >> 8) & 0xFF);
    report_rate.l                            = (uint8_t)(report_rate_ms & 0xFF);
    i2c_status_t status                      = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, selected_reg, (uint8_t *)&report_rate, 2, AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_set_reati(bool enabled, bool end_session) {
    azoteq_iqs5xx_system_config_0_t config = {0};
    i2c_status_t                    status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_0, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_config_0_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        config.reati = enabled;
        status       = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_0, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_config_0_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_set_event_mode(bool enabled, bool end_session) {
    azoteq_iqs5xx_system_config_1_t config = {0};
    i2c_status_t                    status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_1, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_config_1_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        config.event_mode     = enabled;
        config.touch_event    = true;
        config.tp_event       = true;
        config.prox_event     = false;
        config.snap_event     = false;
        config.reati_event    = false;
        config.alp_prox_event = false;
        config.gesture_event  = true;
        status                = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONFIG_1, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_config_1_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_set_gesture_config(bool end_session) {
    azoteq_iqs5xx_gesture_config_t config = {0};
    i2c_status_t                   status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SINGLE_FINGER_GESTURES, (uint8_t *)&config, sizeof(azoteq_iqs5xx_gesture_config_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    pd_dprintf("azo scroll: %d\n", config.multi_finger_gestures.scroll);
    if (status == I2C_STATUS_SUCCESS) {
        config.single_finger_gestures.single_tap     = AZOTEQ_IQS5XX_TAP_ENABLE;
        config.single_finger_gestures.press_and_hold = AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE;
        config.single_finger_gestures.swipe_x_plus   = AZOTEQ_IQS5XX_SWIPE_X_ENABLE;
        config.single_finger_gestures.swipe_x_minus  = AZOTEQ_IQS5XX_SWIPE_X_ENABLE;
        config.single_finger_gestures.swipe_y_plus   = AZOTEQ_IQS5XX_SWIPE_Y_ENABLE;
        config.single_finger_gestures.swipe_y_minus  = AZOTEQ_IQS5XX_SWIPE_Y_ENABLE;
        config.multi_finger_gestures.two_finger_tap  = AZOTEQ_IQS5XX_TWO_FINGER_TAP_ENABLE;
        config.multi_finger_gestures.scroll          = AZOTEQ_IQS5XX_SCROLL_ENABLE;
        config.multi_finger_gestures.zoom            = AZOTEQ_IQS5XX_ZOOM_ENABLE;
        config.tap_time                              = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_TAP_TIME);
        config.tap_distance                          = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_TAP_DISTANCE);
        config.hold_time                             = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_HOLD_TIME);
        config.swipe_initial_time                    = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_SWIPE_INITIAL_TIME);
        config.swipe_initial_distance                = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_SWIPE_INITIAL_DISTANCE);
        config.swipe_consecutive_time                = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_TIME);
        config.swipe_consecutive_distance            = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_SWIPE_CONSECUTIVE_DISTANCE);
        config.scroll_initial_distance               = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_SCROLL_INITIAL_DISTANCE);
        config.zoom_initial_distance                 = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_ZOOM_INITIAL_DISTANCE);
        config.zoom_consecutive_distance             = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(AZOTEQ_IQS5XX_ZOOM_CONSECUTIVE_DISTANCE);
        status                                       = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SINGLE_FINGER_GESTURES, (uint8_t *)&config, sizeof(azoteq_iqs5xx_gesture_config_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_set_xy_config(bool flip_x, bool flip_y, bool switch_xy, bool palm_reject, bool end_session) {
    azoteq_iqs5xx_xy_config_0_t config = {0};
    i2c_status_t                status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_XY_CONFIG_0, (uint8_t *)&config, sizeof(azoteq_iqs5xx_xy_config_0_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        if (flip_x) {
            config.flip_x = !config.flip_x;
        }
        if (flip_y) {
            config.flip_y = !config.flip_y;
        }
        if (switch_xy) {
            config.switch_xy_axis = !config.switch_xy_axis;
        }
        config.palm_reject = palm_reject;
        status             = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_XY_CONFIG_0, (uint8_t *)&config, sizeof(azoteq_iqs5xx_xy_config_0_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

i2c_status_t azoteq_iqs5xx_reset_suspend(bool reset, bool suspend, bool end_session) {
    azoteq_iqs5xx_system_control_1_t config = {0};
    i2c_status_t                     status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONTROL_1, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_control_1_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        config.reset   = reset;
        config.suspend = suspend;
        status         = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_SYSTEM_CONTROL_1, (uint8_t *)&config, sizeof(azoteq_iqs5xx_system_control_1_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
    if (end_session) {
        azoteq_iqs5xx_end_session();
    }
    return status;
}

void azoteq_iqs5xx_set_cpi(uint16_t cpi) {
#ifdef SVAL_AZOTEQ_MULTITOUCH
    // The contact path needs the full resolution, so it stays fixed; the
    // Svalboard wrapper (keyboards/svalboard/azoteq/azoteq.c) applies the
    // pointer DPI to the mouse output instead.
    (void)cpi;
    return;
#endif
    if (azoteq_iqs5xx_product_number != AZOTEQ_IQS5XX_UNKNOWN) {
        azoteq_iqs5xx_resolution_t resolution = {0};
        resolution.x_resolution               = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(MIN(azoteq_iqs5xx_device_resolution_t.resolution_x, AZOTEQ_IQS5XX_INCH_TO_RESOLUTION_X(cpi)));
        resolution.y_resolution               = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(MIN(azoteq_iqs5xx_device_resolution_t.resolution_y, AZOTEQ_IQS5XX_INCH_TO_RESOLUTION_Y(cpi)));
        i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_X_RESOLUTION, (uint8_t *)&resolution, sizeof(azoteq_iqs5xx_resolution_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    }
}

uint16_t azoteq_iqs5xx_get_cpi(void) {
    if (azoteq_iqs5xx_product_number != AZOTEQ_IQS5XX_UNKNOWN) {
        azoteq_iqs5xx_resolution_t resolution = {0};
        i2c_status_t               status     = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_X_RESOLUTION, (uint8_t *)&resolution, sizeof(azoteq_iqs5xx_resolution_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
        if (status == I2C_STATUS_SUCCESS) {
            return AZOTEQ_IQS5XX_RESOLUTION_X_TO_INCH(AZOTEQ_IQS5XX_SWAP_H_L_BYTES(resolution.x_resolution));
        }
    }
    return 0;
}

uint16_t azoteq_iqs5xx_get_product(void) {
    i2c_status_t status = i2c_read_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_PRODUCT_NUMBER, (uint8_t *)&azoteq_iqs5xx_product_number, sizeof(uint16_t), AZOTEQ_IQS5XX_TIMEOUT_MS);
    if (status == I2C_STATUS_SUCCESS) {
        azoteq_iqs5xx_product_number = AZOTEQ_IQS5XX_SWAP_H_L_BYTES(azoteq_iqs5xx_product_number);
    }
    pd_dprintf("AZOTEQ: Product number %u\n", azoteq_iqs5xx_product_number);
    return azoteq_iqs5xx_product_number;
}

void azoteq_iqs5xx_setup_resolution(void) {
#if !defined(AZOTEQ_IQS5XX_RESOLUTION_X) && !defined(AZOTEQ_IQS5XX_RESOLUTION_Y)
    switch (azoteq_iqs5xx_product_number) {
        case AZOTEQ_IQS550:
            azoteq_iqs5xx_device_resolution_t.resolution_x = 3584;
            azoteq_iqs5xx_device_resolution_t.resolution_y = 2304;
            break;
        case AZOTEQ_IQS572:
            azoteq_iqs5xx_device_resolution_t.resolution_x = 2048;
            azoteq_iqs5xx_device_resolution_t.resolution_y = 1792;
            break;
        case AZOTEQ_IQS525:
            azoteq_iqs5xx_device_resolution_t.resolution_x = 1280;
            azoteq_iqs5xx_device_resolution_t.resolution_y = 768;
            break;
        default:
            // shouldn't be here
            azoteq_iqs5xx_device_resolution_t.resolution_x = 0;
            azoteq_iqs5xx_device_resolution_t.resolution_y = 0;
            break;
    }
#endif
#ifdef AZOTEQ_IQS5XX_RESOLUTION_X
    azoteq_iqs5xx_device_resolution_t.resolution_x = AZOTEQ_IQS5XX_RESOLUTION_X;
#endif
#ifdef AZOTEQ_IQS5XX_RESOLUTION_Y
    azoteq_iqs5xx_device_resolution_t.resolution_y = AZOTEQ_IQS5XX_RESOLUTION_Y;
#endif
}

static i2c_status_t azoteq_iqs5xx_init_status = 1;

bool azoteq_iqs5xx_init(void) {
    i2c_init();
    i2c_ping_address(AZOTEQ_IQS5XX_ADDRESS, 1); // wake
    azoteq_iqs5xx_reset_suspend(true, false, true);
    wait_ms(100);
    i2c_ping_address(AZOTEQ_IQS5XX_ADDRESS, 1); // wake
    if (azoteq_iqs5xx_get_product() != AZOTEQ_IQS5XX_UNKNOWN) {
        azoteq_iqs5xx_setup_resolution();
#ifdef SVAL_AZOTEQ_MULTITOUCH
        const uint8_t max_contacts = 5;
        const uint8_t xy_resolution[] = {0x08, 0x00, 0x07, 0x00}; // TPS43: 2048 x 1792
        azoteq_iqs5xx_init_status = i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, 0x066A, &max_contacts, 1, AZOTEQ_IQS5XX_TIMEOUT_MS);
        azoteq_iqs5xx_init_status |= i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_X_RESOLUTION, xy_resolution, sizeof(xy_resolution), AZOTEQ_IQS5XX_TIMEOUT_MS);
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_report_rate(AZOTEQ_IQS5XX_REPORT_RATE, AZOTEQ_IQS5XX_ACTIVE, false);
#else
        azoteq_iqs5xx_init_status = azoteq_iqs5xx_set_report_rate(AZOTEQ_IQS5XX_REPORT_RATE, AZOTEQ_IQS5XX_ACTIVE, false);
#endif
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_report_rate(AZOTEQ_IQS5XX_REPORT_RATE, AZOTEQ_IQS5XX_IDLE, false);
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_report_rate(AZOTEQ_IQS5XX_REPORT_RATE, AZOTEQ_IQS5XX_IDLE_TOUCH, false);

        uint8_t no_timeout = 255;
        azoteq_iqs5xx_init_status |= i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, AZOTEQ_IQS5XX_REG_IDLE_MODE_TIMEOUT, &no_timeout, 1, AZOTEQ_IQS5XX_TIMEOUT_MS); // Don't enter LP1, LP2 states
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_event_mode(AZOTEQ_IQS5XX_EVENT_MODE, false);
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_reati(true, false);
#ifdef SVAL_AZOTEQ_MULTITOUCH
        // SHOW_RESET is latched until acknowledged, even after configuration.
        // Subsequent unexpected resets remain invalid until reinitialization.
        const uint8_t acknowledge_reset = 0x80;
        azoteq_iqs5xx_init_status |= i2c_write_register16(AZOTEQ_IQS5XX_ADDRESS, 0x0431, &acknowledge_reset, 1, AZOTEQ_IQS5XX_TIMEOUT_MS);
#endif
#if defined(AZOTEQ_IQS5XX_ROTATION_90)
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_xy_config(false, true, true, true, false);
#elif defined(AZOTEQ_IQS5XX_ROTATION_180)
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_xy_config(true, true, false, true, false);
#elif defined(AZOTEQ_IQS5XX_ROTATION_270)
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_xy_config(true, false, true, true, false);
#else
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_xy_config(false, false, false, true, false);
#endif
        azoteq_iqs5xx_init_status |= azoteq_iqs5xx_set_gesture_config(true);
        wait_ms(AZOTEQ_IQS5XX_REPORT_RATE + 1);
    }

    return azoteq_iqs5xx_init_status == I2C_STATUS_SUCCESS;
};

report_mouse_t azoteq_iqs5xx_get_report(report_mouse_t mouse_report) {
    // The chip reports a gesture's button only while the gesture lasts: a
    // tap for one read, press and hold until the finger lifts. The driver
    // owns the buttons it pressed and releases them when the chip stops
    // reporting them; any other button that came in (a mouse key, another
    // device, the core passing the last report back in) stays as it was.
    static uint8_t gesture_owned = 0;
    const uint8_t  outside       = mouse_report.buttons & ~gesture_owned;
    uint8_t        gesture       = 0;
    report_mouse_t temp_report   = {0};

    azoteq_iqs5xx_base_data_t base_data       = {0};
    i2c_status_t              status          = azoteq_iqs5xx_get_base_data(&base_data);
    bool                      ignore_movement = false;

    if (status == I2C_STATUS_SUCCESS) {
#ifdef POINTING_DEVICE_DEBUG
        if (base_data.previous_cycle_time > AZOTEQ_IQS5XX_REPORT_RATE) {
            pd_dprintf("IQS5XX - previous cycle time missed, took: %dms\n", base_data.previous_cycle_time);
        }
#endif
        if (base_data.gesture_events_0.single_tap || base_data.gesture_events_0.press_and_hold) {
            pd_dprintf("IQS5XX - Single tap/hold.\n");
            gesture = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON1);
        } else if (base_data.gesture_events_1.two_finger_tap) {
            pd_dprintf("IQS5XX - Two finger tap.\n");
            gesture = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON2);
        } else if (base_data.gesture_events_0.swipe_x_neg) {
            pd_dprintf("IQS5XX - X-.\n");
            gesture         = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON4);
            ignore_movement = true;
        } else if (base_data.gesture_events_0.swipe_x_pos) {
            pd_dprintf("IQS5XX - X+.\n");
            gesture         = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON5);
            ignore_movement = true;
        } else if (base_data.gesture_events_0.swipe_y_neg) {
            pd_dprintf("IQS5XX - Y-.\n");
            gesture         = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON6);
            ignore_movement = true;
        } else if (base_data.gesture_events_0.swipe_y_pos) {
            pd_dprintf("IQS5XX - Y+.\n");
            gesture         = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON3);
            ignore_movement = true;
        } else if (base_data.gesture_events_1.zoom) {
            if (AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.x.h, base_data.x.l) < 0) {
                pd_dprintf("IQS5XX - Zoom out.\n");
                gesture = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON7);
            } else if (AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.x.h, base_data.x.l) > 0) {
                pd_dprintf("IQS5XX - Zoom in.\n");
                gesture = pointing_device_handle_buttons(gesture, true, POINTING_DEVICE_BUTTON8);
            }
        } else if (base_data.gesture_events_1.scroll) {
            pd_dprintf("IQS5XX - Scroll.\n");
            temp_report.h = CONSTRAIN_HID(AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.x.h, base_data.x.l));
            temp_report.v = CONSTRAIN_HID(AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.y.h, base_data.y.l));
        }
        if (base_data.number_of_fingers == 1 && !ignore_movement) {
            temp_report.x = CONSTRAIN_HID_XY(AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.x.h, base_data.x.l));
            temp_report.y = CONSTRAIN_HID_XY(AZOTEQ_IQS5XX_COMBINE_H_L_BYTES(base_data.y.h, base_data.y.l));
        }

    } else {
        // A failed read reports no gesture, so a pad that stops answering
        // cannot leave a button down.
        pd_dprintf("IQS5XX - get report failed, i2c status: %d \n", status);
    }

    temp_report.buttons = outside | gesture;
    gesture_owned       = gesture & ~outside;
    return temp_report;
}
