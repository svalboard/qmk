// Host stand-ins for what keyboards/svalboard/azoteq/touch_stream.c uses from
// QMK and ChibiOS (tests/sval_touch_stream). The test file defines them.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
uint32_t timer_read32(void);
uint32_t timer_elapsed32(uint32_t last);
bool     is_keyboard_master(void);
bool     is_keyboard_left(void);
