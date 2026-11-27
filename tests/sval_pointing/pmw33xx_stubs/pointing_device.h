// Host-test stand-in for QMK's pointing_device.h (pmw33xx_burst_test.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int16_t x, y;
} report_mouse_t;
typedef struct {
    bool (*init)(void);
    report_mouse_t (*get_report)(report_mouse_t);
    void (*set_cpi)(uint16_t);
    uint16_t (*get_cpi)(void);
} pointing_device_driver_t;
#define CONSTRAIN_HID_XY(v) (v)
