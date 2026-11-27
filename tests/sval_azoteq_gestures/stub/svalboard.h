// The part of keyboards/svalboard/svalboard.h that azoteq.c uses.
#pragma once
#include <stdint.h>
enum sval_pointer_sensor {
    SVAL_SENSOR_NONE        = 0,
    SVAL_SENSOR_NO_RESPONSE = 3,
    SVAL_SENSOR_AZOTEQ      = 5,
};
#define SVAL_POINTER_INIT_OK 0x04
typedef struct {
    uint8_t flags, built_for, detected, product_id, inverse_product_id;
} sval_pointer_identity_t;
uint8_t  sval_pad_gestures(void);
uint16_t sval_pad_tuning(void);
