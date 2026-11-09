#pragma once
#include <stdint.h>
typedef struct {
    int16_t x, y;
} report_mouse_t;
#define POINTING_DEVICE_STATUS_UNKNOWN 0
#define POINTING_DEVICE_STATUS_SUCCESS 1
#define POINTING_DEVICE_STATUS_INIT_FAILED 2
extern int sim_status; // QMK's pointing_device_status, owned by the test
static inline void pointing_device_set_status(int s) { sim_status = s; }
static inline int  pointing_device_get_status(void) { return sim_status; }
