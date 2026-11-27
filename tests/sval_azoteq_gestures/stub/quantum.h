// Host stand-ins for what keyboards/svalboard/azoteq/azoteq.c uses from QMK
// (tests/sval_azoteq_gestures). The test file defines the functions.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int16_t i2c_status_t;
#define I2C_STATUS_SUCCESS (0)
#define I2C_STATUS_ERROR (-1)
#define I2C_STATUS_TIMEOUT (-2)
void         i2c_init(void);
i2c_status_t i2c_ping_address(uint8_t address, uint16_t timeout);
i2c_status_t i2c_read_register16(uint8_t devaddr, uint16_t regaddr, uint8_t *data, uint16_t length, uint16_t timeout);
i2c_status_t i2c_write_register16(uint8_t devaddr, uint16_t regaddr, const uint8_t *data, uint16_t length, uint16_t timeout);

void     wait_us(uint32_t us);
uint16_t timer_read(void);
uint32_t timer_read32(void);
uint16_t timer_elapsed(uint16_t last);

#define dprintf(...) ((void)0)
#define uprintf(...) ((void)0)

#define I2C1_SDA_PIN 16
#define I2C1_SCL_PIN 17
bool gpio_read_pin(int pin);
bool is_keyboard_left(void);
