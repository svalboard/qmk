// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What keyboards/svalboard/split_pause.c needs from QMK, for the host tests
// (test_split.c supplies the mocks).

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t matrix_row_t;
#define MATRIX_ROWS 10 // keyboard.json: five rows per half
#define MATRIX_ROWS_PER_HAND (MATRIX_ROWS / 2)
#ifndef POINTING_DEVICE_ENABLE
#    define POINTING_DEVICE_ENABLE
#endif
#ifndef SPLIT_POINTING_ENABLE
#    define SPLIT_POINTING_ENABLE
#endif

typedef struct {
    uint8_t buttons;
    int16_t x, y, v, h;
} report_mouse_t;

bool    matrix_scan_custom(matrix_row_t current_matrix[]);
bool    debounce(matrix_row_t raw[], matrix_row_t cooked[], bool changed);
bool    matrix_post_scan(void);
void    matrix_scan_kb(void);
bool    is_keyboard_master(void);
bool    is_transport_connected(void);
void    split_watchdog_update(bool done);
void    pointing_device_set_shared_report(report_mouse_t report);
uint8_t matrix_scan(void);
bool    update_split_slave_frozen(void);
