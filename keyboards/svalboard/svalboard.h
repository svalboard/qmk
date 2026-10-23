/*
Copyright 2023 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include "quantum.h"
#include "pointing_motion_vs.h"

extern const int16_t mh_timer_choices[6];

struct layer_hsv {
    uint8_t hue;
    uint8_t sat;
    uint8_t val;
};

struct __attribute__((__packed__)) saved_values {
    bool left_scroll :1;
    bool right_scroll :1;
    bool axis_scroll_lock: 1;
    bool auto_mouse: 1;
    bool natural_scroll: 1;
    bool left_automouse: 1;  // Left pointer movement activates the auto mouse layer
    bool right_automouse: 1; // Right pointer movement activates the auto mouse layer
    unsigned int unused0 :1;
    uint8_t left_dpi_index;
    uint8_t right_dpi_index;
    uint8_t mh_timer_index;
    struct layer_hsv layer_colors[DYNAMIC_KEYMAP_LAYER_COUNT];
    uint8_t turbo_scan;
    uint16_t automouse_threshold; // Movement distance required for layer activation (0=disabled)
    uint8_t automouse_decay;      // Accumulator decay time in 10ms units (0=no decay)
    uint16_t scan_prewait_us;     // Row-on settle time before reading (0 = use turbo table)
    uint16_t scan_postwait_us;    // Row-off recovery time after reading (0 = use turbo table)
    uint16_t scan_period_us;      // Frame period while active (0 = unpaced, scan back to back)
    uint16_t scan_idle_period_ms; // Light idle: frame period in ms once idle (<= active = no change)
    uint16_t scan_idle_after_ms;  // Light idle: timeout since last raw matrix change (0 = never)
    uint16_t scan_deep_after_s;   // Deep idle: timeout in seconds (0 = never)
    uint16_t scan_deep_period_ms; // Deep idle: frame period in ms (<= active = no change)
    uint8_t  idle_flags;          // SVAL_IDLE_* bits: pointer rest, RGB dim, CPU sleep, low clock, long naps
    uint8_t  scan_deep_clock_idx; // deep-idle clock when SVAL_IDLE_LOW_CLOCK: 0 = 48 MHz, 1 = 24 MHz, 2 = 12 MHz
};

// RPC structure for split keyboard sync
typedef struct __attribute__((__packed__)) _presence_rpc_t {
    uint8_t  turbo_scan;
    uint16_t scan_prewait_us;
    uint16_t scan_postwait_us;
    uint16_t scan_period_us;
    uint16_t scan_idle_period_ms;
    uint16_t scan_idle_after_ms;
    uint16_t scan_deep_after_s;
    uint16_t scan_deep_period_ms;
    uint8_t  idle_flags;
    uint8_t  scan_deep_clock_idx;
#ifdef SVAL_UPDATER
    // The updater's light (update_light_t, updater/updater.h), appended (D24):
    // the half without USB shows the same one. The slave reads it only when
    // the ping is long enough: a master from before it sends 18 bytes, and an
    // older slave never reads past its own fields.
    uint8_t update_light;
#endif
} presence_rpc_t;

// Which pointing module a half's firmware drives and which one answered, so a host
// can say when the wrong .uf2 was flashed (Sval 0x2C, docs/protocol.md).
enum sval_pointer_sensor {
    SVAL_SENSOR_NONE        = 0, // built_for: no pointing module; detected: not checked yet
    SVAL_SENSOR_PMW3360     = 1, // trackball
    SVAL_SENSOR_PMW3389     = 2, // trackball
    SVAL_SENSOR_NO_RESPONSE = 3, // detected only: no module answered
    SVAL_SENSOR_UNKNOWN     = 4, // detected only: the trackball bus answered with another Product_ID
    SVAL_SENSOR_AZOTEQ      = 5, // trackpad (IQS5xx)
    SVAL_SENSOR_TRACKPOINT  = 6, // TrackPoint (PS/2)
};
#define SVAL_POINTER_REPORTED 0x01 // this block holds a report
#define SVAL_POINTER_LEFT     0x02 // the half is the left half (EE_HANDS)
#define SVAL_POINTER_INIT_OK  0x04 // the sensor driver initialised
#define SVAL_POINTER_AUTO     0x08 // the firmware drives whichever module is attached; built_for is the one it chose
#define SVAL_POINTER_OS_TOUCHPAD 0x10 // the OS drives this pad as a touchpad (native touchpad builds): pointer settings do not apply
#define SVAL_POINTER_SIDE_UNKNOWN 0x20 // either-hand builds: the half has no side yet, and SVAL_POINTER_LEFT means nothing
typedef struct __attribute__((__packed__)) {
    uint8_t flags;              // SVAL_POINTER_*
    uint8_t built_for;          // enum sval_pointer_sensor
    uint8_t detected;           // enum sval_pointer_sensor
    uint8_t product_id;         // raw IDs: PMW Product_ID, Azoteq product number high byte, or PS/2 device ID
    uint8_t inverse_product_id; // PMW Inverse_Product_ID, or Azoteq product number low byte
} sval_pointer_identity_t;
// Each pointing build fills built_for, detected (its own module, or
// SVAL_SENSOR_NO_RESPONSE), the raw IDs and SVAL_POINTER_INIT_OK; the weak default
// leaves it zeroed (no module). When the build's own module didn't answer,
// the search (pointer_reprobe.c) probes the other buses, at boot and up to 30 s after while
// nothing works, and reports what it found.
void sval_pointer_identity(sval_pointer_identity_t *id);
// The bus the build's own driver uses (SVAL_BUS_*), which the probes skip; 0 for no module.
uint8_t sval_pointer_own_bus(void);

// Pointing modes for POINTING_MOTION (Sval 0x2D). global_bits uses SVAL_MOTION_* from
// pointing_motion.h; axis_state is 0 free, 1 locked vertical, 2 locked horizontal.
// keymaps/keymap_support.c fills it; the weak default reports nothing active.
// has_vs: the build has Velocity Scroll, so the reply carries its block (vs).
typedef struct {
    uint8_t          global_bits;
    uint8_t          axis_state;
    bool             left_scroll, right_scroll, left_automouse, right_automouse;
    bool             has_vs;
    sval_motion_vs_t vs;
} sval_pointing_modes_t;
void sval_pointing_modes(sval_pointing_modes_t *m);

// The other half's reply to the master's KEYBOARD_SYNC_A ping. The magic tells
// it apart from a stale RPC buffer left by firmware that doesn't fill it.
#define SVAL_PRESENCE_RSP_MAGIC0 0x53 // 'S'
#define SVAL_PRESENCE_RSP_MAGIC1 0x50 // 'P'
typedef struct __attribute__((__packed__)) {
    uint8_t                 magic[2];
    sval_pointer_identity_t pointer;
#ifdef SVAL_UPDATER
    uint8_t update[17]; // the updater's update_presence_t (D17), appended
#endif
} presence_rsp_t;

#define SVAL_TURBO_CHOICES 7

// Hardware revision, read once from SVAL_HW_REV_PIN (internal pull-up; the
// board straps the pin to ground on revision B "flipfet").
enum sval_hw_rev {
    SVAL_HW_REV_A       = 0,
    SVAL_HW_REV_FLIPFET = 1,
};
uint8_t sval_hw_rev(void);
bool    sval_hw_rev_is_flipfet(void);

// Thumb handedness strap, read once at boot from SVAL_THUMB_HAND_PIN (the
// WS2812 data pin) with the thumb row on. A left thumb with R7 (10k LED_DIN
// to GND) reads low under both pulls; a right thumb with R7 (10k LED_DIN to
// the thumb row) reads high under both. DEFAULT is not "right": it is also an
// unmodified thumb, no thumb, or a thumb on the 8-pin cable.
enum sval_thumb_hand {
    SVAL_THUMB_HAND_DEFAULT    = 0, // pull-up 1, pull-down 0: no strap
    SVAL_THUMB_HAND_LEFT       = 1, // pull-up 0, pull-down 0: TCL R7 fitted
    SVAL_THUMB_HAND_ANOMALY    = 2, // pull-up 0, pull-down 1: no strap does this
    SVAL_THUMB_HAND_SKIPPED    = 3, // not probed (Pimoroni build: GP19 is I2C1 SCL)
    SVAL_THUMB_HAND_RIGHT      = 4, // pull-up 1, pull-down 1 (row on): TCR R7 fitted
    SVAL_THUMB_HAND_NOT_PROBED = 0xFF,
};
uint8_t sval_thumb_hand(void);
// Raw readings: bit 0 = pull-down read (row on), bit 1 = pull-up majority
// (row on), bit 2 = pull-down read with the row off (diagnosis only),
// bits 4-7 = how many of the 8 pull-up reads were high.
uint8_t sval_thumb_hand_raw(void);
bool    sval_other_half_connected(void);
uint8_t sval_pad_hands(void); // hands whose pointing module is an Azoteq pad (bit 0 left, bit 1 right)
void    sval_scan_timing(uint16_t *pre, uint16_t *post); // matrix.c
uint8_t sval_pushed_mask(bool thumbs);                    // matrix.c
void    sval_row_drive_raw(uint8_t row, bool on);         // matrix.c: no critical section
uint32_t sval_scan_period_now(uint8_t *stage);            // matrix.c: effective frame period us (0 = unpaced); stage 0/1/2
void    sval_scan_stats(uint32_t *frame_us, uint16_t *led_us, uint8_t *stage); // matrix.c: measured

typedef struct saved_values saved_values_t;

extern saved_values_t global_saved_values;
void svalboard_saved_values_defaults(void); // factory defaults into global_saved_values (RAM only)
void svalboard_eeprom_set_valid(void);       // stamp the stored saved_values as current
void output_keyboard_info(void);
void increase_left_dpi(void);
void increase_right_dpi(void);
void decrease_left_dpi(void);
void decrease_right_dpi(void);
void set_left_dpi(uint8_t index);
void set_right_dpi(uint8_t index);
void set_dpi_from_eeprom(void);
void write_eeprom_kb(void);
void read_eeprom_kb(void);
void change_turbo_scan(void);

// Idle power diagnostics, reported by Scan Lab op 0x12.
typedef struct {
    uint8_t sensor_present;  // 1 if this half has a PMW33xx sensor
    uint8_t sensor_mode;     // last Motion byte OP_MODE: 0 run, 1 rest1, 2 rest2, 3 rest3; bit7 valid, bit6 lifted
    uint8_t sensor_config2;  // last value written to Config2 (0x20 = rest enabled)
    uint8_t rgb_val_now;
    uint8_t rgb_val_awake;
    uint8_t rgb_stage;       // 0 awake, 1 dimmed, 2 off
    uint8_t rgb_enabled;
} sval_idle_status_t;
void sval_idle_status(sval_idle_status_t *st);
void sval_pointer_status(sval_idle_status_t *st); // weak: fills the sensor_* fields

// Deep-idle clock and nap control (power.c).
void     sval_clock_low(void);        // clk_sys/clk_peri from the USB PLL at 48 MHz, system PLL off, PIO dividers rescaled
void     sval_clock_full(void);       // back to 125 MHz (no-op when already there)
bool     sval_clock_is_low(void);
uint8_t  sval_clock_mhz(void);        // clock in force now
uint8_t  sval_deep_clock_mhz(void);   // configured deep-idle clock (48/24/12)
bool     sval_rgb_idle_quiesced(void); // master: RGB has reached its deep-idle state and the last write is done
uint32_t sval_deep_nap_us(void);      // longest nap allowed in deep idle on this half
void     sval_sleep_gating_init(void); // stop clocking unused blocks while the core is in WFI
bool     sval_host_recent(void);       // a host app sent a raw HID packet within SVAL_HOST_ACTIVE_MS
uint32_t sval_host_idle_ms(void);     // ms since the last host packet (UINT32_MAX until the first one)

// Idle power features. Flags live in global_saved_values.idle_flags on both halves.
void sval_pointer_rest_apply(void);   // push SVAL_IDLE_POINTER_REST into the sensor (weak no-op without one)
void sval_rgb_idle_task(void);        // master: dim/restore the RGB by idle stage
uint8_t sval_rgb_awake_val(void);     // RGB brightness as it was before any idle dimming
void recalibrate_pointer(void);
void sval_set_active_layer(uint32_t layer, bool save);
void sval_on_reconnect(void);
int16_t get_left_dpi(void);
int16_t get_right_dpi(void);
