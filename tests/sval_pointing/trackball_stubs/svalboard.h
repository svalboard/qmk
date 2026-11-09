// Host-test stand-in for svalboard.h, only what trackball/trackball.c uses.
#pragma once
#include <stdint.h>

enum sval_pointer_sensor { SVAL_SENSOR_NONE = 0, SVAL_SENSOR_PMW3360 = 1, SVAL_SENSOR_PMW3389 = 2, SVAL_SENSOR_NO_RESPONSE = 3 };
#define SVAL_POINTER_INIT_OK 0x04
typedef struct __attribute__((__packed__)) {
    uint8_t flags, built_for, detected, product_id, inverse_product_id;
} sval_pointer_identity_t;
typedef struct {
    uint8_t sensor_present, sensor_mode, sensor_config2;
} sval_idle_status_t;
#define SVAL_IDLE_POINTER_REST 0x01
struct {
    uint8_t idle_flags;
} global_saved_values;
