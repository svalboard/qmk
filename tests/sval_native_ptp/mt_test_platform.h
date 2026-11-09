// Host stand-ins for azoteq/multitouch.c (SVAL_MT_HOST_TEST): a settable clock,
// the split link (one RPC handler for the other half, set by the test), the
// I2C error bits, the pointing identity of each hand and the activity hook.
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAW_EPSIZE 64
#define KEYBOARD_SYNC_B 16
#define I2C_STATUS_SUCCESS 0
#define I2C_STATUS_ERROR -1
#define I2C_STATUS_TIMEOUT -2
typedef int i2c_status_t;

static uint32_t clock_ms, mt_test_us;
static bool     master, connected, busy, left_half = false;
static uint8_t  hands_with_pad = 3;
static unsigned rpc_calls, activity_calls, reinit_calls;
// The other half: what its RPC answers (NULL: the call fails).
static void (*other_half)(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);

static bool is_keyboard_master(void) {
    return master;
}
static bool is_keyboard_left(void) {
    return left_half;
}
static bool is_transport_connected(void) {
    return connected;
}
#define updater_busy() (busy)
static uint32_t timer_read32(void) {
    return clock_ms;
}
static uint32_t timer_elapsed32(uint32_t t) {
    return clock_ms - t;
}
static uint32_t mt_now_us(void) {
    return mt_test_us;
}
static void osalSysLock(void) {}
static void osalSysUnlock(void) {}
// Split transactions that fail: each one blocks the main loop for the split
// timeout first, as QMK's serial transport does (SERIAL_USART_TIMEOUT, 20 ms).
static unsigned rpc_fail_next, rpc_failed;
static uint32_t rpc_fail_ms = 20;
static void (*rpc_blocked)(uint32_t ms); // the test moves every clock on
static bool transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    assert(id == KEYBOARD_SYNC_B && out_len == 32 && in_len >= 2);
    rpc_calls++;
    if (rpc_fail_next) {
        rpc_fail_next--;
        rpc_failed++;
        if (rpc_blocked) rpc_blocked(rpc_fail_ms);
        return false;
    }
    if (!connected || !other_half) return false;
    other_half(in_len, (const uint8_t *)in, out_len, (uint8_t *)out);
    return true;
}
static uint32_t test_i2c_errors;
static int      I2C_DRIVER;
static uint32_t i2cGetErrors(const int *d) {
    (void)d;
    return test_i2c_errors;
}
uint8_t sval_pad_hands(void) {
    return hands_with_pad;
}
static void last_pointing_device_activity_trigger(void) {
    activity_calls++;
}
static void sval_azoteq_restart(void) {
    reinit_calls++;
}
