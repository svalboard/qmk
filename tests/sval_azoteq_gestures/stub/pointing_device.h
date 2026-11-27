#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint8_t buttons;
    int16_t x, y, h, v;
} report_mouse_t;
typedef enum { POINTING_DEVICE_STATUS_UNKNOWN, POINTING_DEVICE_STATUS_SUCCESS } pointing_device_status_t;
void pointing_device_set_status(pointing_device_status_t status);
