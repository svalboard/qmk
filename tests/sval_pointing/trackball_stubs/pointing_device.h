#pragma once
#include <stdint.h>
typedef struct {
    int16_t x, y;
} report_mouse_t;
#define POINTING_DEVICE_STATUS_SUCCESS 1
static inline void pointing_device_set_status(int s) { (void)s; }
