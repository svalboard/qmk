// The pointer identity, as keyboards/svalboard/svalboard.h has it.
#pragma once
#include <stdint.h>
enum sval_pointer_sensor {
    SVAL_SENSOR_NONE        = 0,
    SVAL_SENSOR_NO_RESPONSE = 3,
    SVAL_SENSOR_TRACKPOINT  = 6,
};
#define SVAL_POINTER_INIT_OK 0x04
typedef struct {
    uint8_t flags, built_for, detected, product_id, inverse_product_id;
} sval_pointer_identity_t;
void sval_pointer_identity(sval_pointer_identity_t *id);
