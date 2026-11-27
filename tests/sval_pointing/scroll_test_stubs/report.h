// Host stand-in for QMK's report.h: the extended mouse report's fields.
#pragma once
#include <stdint.h>
typedef int16_t mouse_xy_report_t;
typedef int16_t mouse_hv_report_t;
typedef struct {
    uint8_t           buttons;
    mouse_xy_report_t x;
    mouse_xy_report_t y;
    mouse_hv_report_t v;
    mouse_hv_report_t h;
} report_mouse_t;
