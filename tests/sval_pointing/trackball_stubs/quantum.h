// Host-test stand-in for QMK's quantum.h, only what trackball/trackball.c uses.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONSTRAIN(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CONSTRAIN_HID_XY(v) (v)
#define dprintf(...) printf(__VA_ARGS__)

extern uint32_t sim_ms;
extern uint32_t sim_stall_us; // wait_us time spent, to see how long the main loop is held
static inline uint32_t timer_read32(void) { return sim_ms; }
static inline uint32_t timer_elapsed32(uint32_t t) { return sim_ms - t; }
static inline void     wait_us(uint32_t us) { sim_stall_us += us; }

#define SPI_SCK_PIN 0
#define SPI_MOSI_PIN 0
#define SPI_MISO_PIN 0
#define SPI_SCK_FLAGS 0
#define SPI_MOSI_FLAGS 0
#define SPI_MISO_FLAGS 0
#define PAL_PORT(x) 0
#define PAL_PAD(x) 0
#define palSetPadMode(a, b, c) ((void)0)
static inline void spi_init(void) {}
static inline void spi_stop(void) {}
