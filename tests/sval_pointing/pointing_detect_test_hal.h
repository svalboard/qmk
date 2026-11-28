// Host test pins for pointing_detect.c: a simulated connector with wired lines and pull-ups,
// and simulated devices (tests/sval_pointing/sim.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t pin_t;
enum { GP16 = 16, GP17 = 17, GP23 = 23, GP24 = 24, GP25 = 25, GP26 = 26, GP27 = 27 };

enum sval_pointer_sensor {
    SVAL_SENSOR_NONE        = 0,
    SVAL_SENSOR_PMW3360     = 1,
    SVAL_SENSOR_PMW3389     = 2,
    SVAL_SENSOR_NO_RESPONSE = 3,
    SVAL_SENSOR_UNKNOWN     = 4,
    SVAL_SENSOR_AZOTEQ      = 5,
    SVAL_SENSOR_TRACKPOINT  = 6,
};

void pin_release(pin_t pin);
void pin_low(pin_t pin);
void pin_drive(pin_t pin, bool high);
bool pin_read(pin_t pin);
void det_wait_us(uint32_t us);
