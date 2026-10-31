// Pins and settings for every pointing module, as the per-module builds set them.
#pragma once

// Trackball (PMW3360/PMW3389): SPI1
#define SPI_DRIVER SPID1
#define SPI_SCK_PIN GP26
#define SPI_MOSI_PIN GP27
#define SPI_MISO_PIN GP24
#define PMW33XX_CS_PIN GP25

// Azoteq trackpad (IQS5xx): I2C0
#define I2C_DRIVER I2CD0
#define I2C1_SDA_PIN GP16
#define I2C1_SCL_PIN GP17
#define AZOTEQ_IQS5XX_TPS43
#define AZOTEQ_IQS5XX_TIMEOUT_MS 10
#define AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE true
#define AZOTEQ_IQS5XX_HOLD_TIME 300 // press and hold, ms (QMK's driver default, 0x12C)

// TrackPoint: PS/2 on PIO1
#define PS2_PIO_USE_PIO1
#define PS2_CLOCK_PIN GP24
#define PS2_DATA_PIN GP23
#define PS2_MOUSE_SCROLL_DIVISOR_H 4
#define PS2_MOUSE_SCROLL_DIVISOR_V 4
// The probe has already heard from it, so its power-on self-test is done.
#define PS2_MOUSE_INIT_DELAY 100

// Native Precision Touchpad (SVAL_NATIVE_PTP): its build settings.
#include "../azoteq/ptp_config.h"
