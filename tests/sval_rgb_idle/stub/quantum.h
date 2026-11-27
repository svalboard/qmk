// Mock QMK surface for tests/sval_rgb_idle/test_rgb_idle.c: rgblight (with its
// EEPROM copy), VIA's lighting channel, the clock and USB.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DYNAMIC_KEYMAP_LAYER_COUNT 16
#define MATRIX_ROWS 10
#define MATRIX_COLS 6
#define RGBLIGHT_ENABLE
#define RGBLIGHT_SLEEP
#define VIA_ENABLE
#define RGBLIGHT_LIMIT_VAL 255
#define SVAL_IDLE_RGB_DIM 0x02
#define SVAL_IDLE_RGB_LIGHT_DIV 4

typedef uint8_t matrix_row_t;
typedef struct {
    uint8_t col, row;
} keypos_t;

uint32_t timer_read32(void);
uint32_t timer_elapsed32(uint32_t t);
uint32_t last_input_activity_elapsed(void);
bool     is_keyboard_master(void);

// rgblight (quantum/rgblight/rgblight.h)
uint8_t rgblight_get_val(void);
uint8_t rgblight_get_hue(void);
uint8_t rgblight_get_sat(void);
bool    rgblight_is_enabled(void);
void    rgblight_enable_noeeprom(void);
void    rgblight_disable_noeeprom(void);
void    rgblight_sethsv_noeeprom(uint8_t h, uint8_t s, uint8_t v);
void    rgblight_suspend(void);
void    rgblight_wakeup(void);
