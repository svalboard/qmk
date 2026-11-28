// Host test stand-ins for what trackpoint.c gets from QMK (trackpoint_test.c
// supplies the functions).
#pragma once
#include <stdbool.h>
#include <stdint.h>
void wait_ms(uint32_t ms);
