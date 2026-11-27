// Host-test stand-in for QMK's spi_master.h; pmw33xx_burst_test.c simulates the bus.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t pin_t;
typedef int16_t spi_status_t;
#define SPI_STATUS_SUCCESS 0
void         spi_init(void);
bool         spi_start(pin_t pin, bool lsb_first, uint8_t mode, uint16_t divisor);
spi_status_t spi_write(uint8_t data);
spi_status_t spi_read(void);
spi_status_t spi_transmit(const uint8_t *data, uint16_t length);
spi_status_t spi_receive(uint8_t *data, uint16_t length);
void         spi_stop(void);
