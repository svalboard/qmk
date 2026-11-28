#pragma once
#include <stdint.h>
typedef int16_t mouse_xy_report_t;
typedef struct {
    uint8_t           buttons;
    mouse_xy_report_t x, y;
    int8_t            v, h;
} report_mouse_t;
