#pragma once

#define I2C_DRIVER I2CD0
#define I2C1_SDA_PIN GP16
#define I2C1_SCL_PIN GP17

#define AZOTEQ_IQS5XX_TPS43
#define AZOTEQ_IQS5XX_TIMEOUT_MS 10
#define AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE true
//This decreases the required 'wait' time to activate dragging/selecting on a touchpad from default 300ms to 20ms. Big QoL. 
#define AZOTEQ_IQS5XX_HOLD_TIME 300 // press and hold, ms (QMK's driver default, 0x12C)
//#define POINTING_DEVICE_MOTION_PIN GP18

#define SPLIT_POINTING_ENABLE
#define POINTING_DEVICE_COMBINED
#define POINTING_DEVICE_AUTO_MOUSE_MH_ENABLE

// Native Precision Touchpad (SVAL_NATIVE_PTP): its build settings.
#include "ptp_config.h"
