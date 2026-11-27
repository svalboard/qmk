// Mock firmware surface for tests/sval_side/test_side_update.c: what the
// firmware part of keyboards/svalboard/side.c calls. Every header side.c
// includes there (but side.h, identity.h, build_reset.h and link_light.h,
// which are the real ones) is a stub in this folder that includes this file.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MATRIX_ROWS 10
#define KEYBOARD_SYNC_B 1
typedef uint8_t pin_t;
#define GP0 0
#define GP1 1
typedef struct {
    struct {
        struct {
            uint8_t row, col;
        } key;
        bool pressed;
        int  type;
    } event;
} keyrecord_t;
#define IS_KEYEVENT(e) ((e).type == 1)

// The die's top 64 KB (0xFF0000-0xFFFFFF): the side sectors, as XIP reads them.
extern uint8_t sim_top[0x10000];
#define XIP_NOCACHE_NOALLOC_BASE ((uintptr_t)sim_top - 0xFF0000u)
typedef struct {
    uint32_t gpio_in;
} sio_hw_t;
extern sio_hw_t *sio_hw;

uint32_t timer_read32(void);
#define timer_expired32(current, future) ((uint32_t)((current) - (future)) < UINT32_MAX / 2)
#define timer_elapsed32(last) ((uint32_t)(timer_read32() - (last)))
void     wait_us(uint32_t us);
void     gpio_set_pin_input_high(pin_t pin);
void     soft_reset_keyboard(void);
void     clear_keyboard(void);
bool     is_keyboard_master(void);
bool     is_transport_connected(void);
bool     pre_process_record_user(uint16_t keycode, keyrecord_t *record);
bool     transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out);
uint16_t eeprom_read_word(const uint16_t *addr);
uint8_t  eeprom_read_byte(const uint8_t *addr);
uint8_t  sval_thumb_hand(void);
uint8_t  sval_thumb_hand_raw(void);
bool     sval_other_half_connected(void);
bool     sval_typing_pause_pre_process(keyrecord_t *record);
bool     sval_split_paused(void);
bool     updater_active(void);
