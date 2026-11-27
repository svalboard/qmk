// Mock QMK surface for tests/sval_scanlab/test_scanlab.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MATRIX_ROWS 10
#define MATRIX_COLS 6
#define KEYBOARD_SYNC_B 1
#define CH_CFG_ST_FREQUENCY 1000000
#define NO_PIN 0xFF
#define SVAL_HOST_BOOTLOADER 0
typedef uint8_t matrix_row_t;
typedef uint8_t pin_t;
bool            is_keyboard_master(void);
bool            is_keyboard_left(void);
bool            transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out);
bool            gpio_read_pin(pin_t pin);
uint32_t        chVTGetSystemTimeX(void);
void            wait_us(uint32_t us);
void            wait_ms(uint32_t ms);
uint32_t        timer_read32(void);
uint32_t        timer_elapsed32(uint32_t since);
void            reset_keyboard(void);
uint32_t        last_input_activity_elapsed(void);
uint32_t        last_matrix_activity_elapsed(void);
uint32_t        last_pointing_device_activity_elapsed(void);
// The test counts the kernel locks instead (chSysLock/chSysUnlock on the board).
extern int            scanlab_test_lock_depth;
static inline uint8_t scanlab_test_lock(void) {
    scanlab_test_lock_depth++;
    return 1;
}
static inline void scanlab_test_unlock(const uint8_t *s) {
    (void)s;
    scanlab_test_lock_depth--;
}
#define ATOMIC_BLOCK_FORCEON for (uint8_t scanlab_test_held __attribute__((__cleanup__(scanlab_test_unlock))) = scanlab_test_lock(), todo = 1; todo; todo = 0)
