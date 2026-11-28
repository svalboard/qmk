// Host-test stand-in for QMK's progmem.h (pmw33xx_burst_test.c).
#pragma once
#include <string.h>
#define PROGMEM
#define memcmp_P memcmp
