// Mock of keyboards/svalboard/svalboard.h for tests/sval_scanlab/test_scanlab.c:
// only what scanlab.c uses.
#pragma once
#include "quantum.h"
typedef struct {
    uint16_t scan_prewait_us, scan_postwait_us, scan_period_us;
    uint16_t scan_idle_period_ms, scan_idle_after_ms, scan_deep_after_s, scan_deep_period_ms;
    uint8_t  idle_flags;
} saved_values_t;
extern saved_values_t global_saved_values;
typedef struct {
    uint8_t sensor_present, sensor_mode, sensor_config2, rgb_val_now, rgb_val_awake, rgb_stage, rgb_enabled;
} sval_idle_status_t;
void     sval_idle_status(sval_idle_status_t *st);
uint8_t  sval_hw_rev(void);
bool     sval_other_half_connected(void);
void     sval_scan_timing(uint16_t *pre, uint16_t *post);
uint8_t  sval_pushed_mask(bool thumbs);
void     sval_row_drive_raw(uint8_t row, bool on);
uint32_t sval_scan_period_now(uint8_t *stage);
void     sval_scan_stats(uint32_t *frame_us, uint16_t *led_us, uint8_t *stage);
uint8_t  sval_clock_mhz(void);
uint8_t  sval_deep_clock_mhz(void);
uint8_t  sval_timing_preset(uint16_t pre, uint16_t post);
