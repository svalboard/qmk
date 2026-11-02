// Mock QMK surface for tests/sval_key_log/test_key_log.c: the key record, the
// debug flag and a console that keeps what is printed.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t col, row;
} keypos_t;
typedef struct {
    keypos_t key;
    uint16_t time;
    uint8_t  type;
    bool     pressed;
} keyevent_t;
typedef struct {
    bool    interrupted : 1;
    bool    reserved2 : 1;
    bool    reserved1 : 1;
    bool    reserved0 : 1;
    uint8_t count : 4;
} tap_t;
typedef struct {
    keyevent_t event;
    tap_t      tap;
} keyrecord_t;

// QMK's debug_enable is debug_config.enable (quantum/logging/debug.h).
extern bool debug_enable;

// The console: QMK's uprintf goes to the console HID interface.
int console_printf(const char *fmt, ...);
#define uprintf(...) console_printf(__VA_ARGS__)
