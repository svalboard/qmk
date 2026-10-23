#include "svalboard.h"
#include "pointing_detect.h"
#include "pointer_reprobe.h"
#if VIA_ENABLE
#    include "via.h"
#endif
#include "version.h"
#include "layout_stamp.h"
#include "split_common/transactions.h"
#include <string.h>
#include QMK_KEYBOARD_H
#include "usb_main.h"
#include "suspend.h"
#include "scanlab.h"
#include "keytest.h"
#include "sval.h"
#include "pointing_motion.h" // after sval.h: needs the generated fragment ids
#include "identity.h"
#include "build_reset.h"
#include "nvm_via.h"
#ifdef SVAL_EITHER_HAND
#    include "side.h"
#endif
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    include "azoteq/multitouch.h"
#endif
#ifdef SVAL_NATIVE_PTP
#    include "azoteq/ptp.h"
#endif
#ifdef SVAL_PERF_STATS
#    include "perf.h"
#endif
#ifdef SVAL_UPDATER
#    include "updater/updater.h"
#    include "updater/update_split.h"
#    include "split_pause.h"
#    include "split_common/transport.h"
_Static_assert(sizeof(((presence_rsp_t *)0)->update) == UPDATE_PRESENCE_BYTES, "presence reply: update_presence_t");
_Static_assert(sizeof(presence_rsp_t) <= RPC_S2M_BUFFER_SIZE, "presence reply fits the split RPC buffer");
_Static_assert(sizeof(presence_rpc_t) <= RPC_M2S_BUFFER_SIZE, "presence ping fits the split RPC buffer");
#endif

// USB remote wakeup status bit (from USB spec, not exported by QMK headers)
#ifndef USB_GETSTATUS_REMOTE_WAKEUP_ENABLED
#    define USB_GETSTATUS_REMOTE_WAKEUP_ENABLED (2U)
#endif

saved_values_t global_saved_values;
const int16_t  mh_timer_choices[6] = {200, 300, 400, 500, 800, -1}; // -1 is infinite.

uint8_t sval_active_layer = 0;

static int8_t hw_rev_cache = -1;
uint8_t       sval_hw_rev(void) {
    if (hw_rev_cache < 0) {
#if defined(SVAL_HW_REV_FORCE)
        hw_rev_cache = SVAL_HW_REV_FORCE;
#else
        gpio_set_pin_input_high(SVAL_HW_REV_PIN);
        wait_us(200);
        hw_rev_cache = gpio_read_pin(SVAL_HW_REV_PIN) ? SVAL_HW_REV_A : SVAL_HW_REV_FLIPFET;
#endif
    }
    return (uint8_t)hw_rev_cache;
}

bool sval_hw_rev_is_flipfet(void) {
    return sval_hw_rev() == SVAL_HW_REV_FLIPFET;
}

// Thumb handedness strap. A left thumb (TCL Gen 2) with R7 fitted has 10k
// from LED_DIN to GND; a right thumb (TCR) with its R7 fitted has 10k from
// LED_DIN to the thumb row. LED_DIN is the WS2812 data pin. Read once at
// boot, before the LED driver, the matrix and split init touch the pins.
//
// The thumb row is switched on for the read, so a right thumb's pull-up has a
// row to pull to. GPIO8 drives a P-channel FET's gate directly on every main
// board (V3.2, CV3.3, Mini1.0): low is on. On V3.2 the FET is reversed and its
// body diode keeps the row near 2.6 V even when off; on CV3.3 and Mini1.0 the
// row floats when off. The row-off pull-down read is kept for diagnosis only.
static uint8_t thumb_hand_class = SVAL_THUMB_HAND_NOT_PROBED;
static uint8_t thumb_hand_raw   = 0;

static void sval_thumb_hand_probe(void) {
#if defined(POINTING_DEVICE_IS_PIMORONI)
    // GP19 is I2C1 SCL on the Pimoroni build: no probe.
    thumb_hand_class = SVAL_THUMB_HAND_SKIPPED;
#else
    const pin_t row = SVAL_THUMB_ROW_PIN;
    gpio_set_pin_output(row);
    gpio_write_pin_high(row); // thumb row off
    gpio_set_pin_input_low(SVAL_THUMB_HAND_PIN);
    wait_us(200);
    const uint8_t down_off = gpio_read_pin(SVAL_THUMB_HAND_PIN) ? 1 : 0;

    gpio_write_pin_low(row); // thumb row on
    wait_us(200);
    const uint8_t down = gpio_read_pin(SVAL_THUMB_HAND_PIN) ? 1 : 0;

    gpio_set_pin_input_high(SVAL_THUMB_HAND_PIN);
    wait_us(200);
    uint8_t ones = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (i) wait_us(10);
        ones += gpio_read_pin(SVAL_THUMB_HAND_PIN) ? 1 : 0;
    }
    const uint8_t up = ones >= 5 ? 1 : 0; // majority of 8 (4/8 counts as low)

    gpio_set_pin_input(SVAL_THUMB_HAND_PIN); // plain input, no pull
    gpio_write_pin_high(row);                // thumb row off; matrix_init() takes the pin over

    if (!up && !down) {
        thumb_hand_class = SVAL_THUMB_HAND_LEFT;
    } else if (up && down) {
        thumb_hand_class = SVAL_THUMB_HAND_RIGHT;
    } else if (up && !down) {
        thumb_hand_class = SVAL_THUMB_HAND_DEFAULT; // no strap: never means right
    } else {
        thumb_hand_class = SVAL_THUMB_HAND_ANOMALY;
    }
    thumb_hand_raw = (uint8_t)(down | (up << 1) | (down_off << 2) | (ones << 4));
#endif
}

uint8_t sval_thumb_hand(void) {
    return thumb_hand_class;
}

uint8_t sval_thumb_hand_raw(void) {
    return thumb_hand_raw;
}

// Store svalboard data in VIA custom config at offset 0
#define SVALBOARD_VIA_CONFIG_OFFSET 0
#define SVALBOARD_VIA_CONFIG_SIZE sizeof(saved_values_t)

// Bump only when a saved_values field is reinterpreted without the struct
// changing size (renamed, reordered or rescaled in place).
#ifndef SVALBOARD_SAVED_VALUES_SCHEMA
#    define SVALBOARD_SAVED_VALUES_SCHEMA 0
#endif

// Magic bytes for EEPROM validation: a layout stamp over the saved_values
// geometry, so pointer, layer-colour and scan settings reset only when they
// would otherwise be misread. It used to be the build timestamp to the second,
// which reset them on every firmware update.
//
// The first two bytes are fixed and are not valid BCD, so an old timestamp magic
// can never be mistaken for a stamp.
#define SVALBOARD_MAGIC_SIZE 6
#define SVALBOARD_MAGIC_OFFSET (SVALBOARD_VIA_CONFIG_OFFSET + SVALBOARD_VIA_CONFIG_SIZE)

#if VIA_ENABLE
static void svalboard_get_magic(uint8_t *magic) {
    const uint32_t values[] = {
        SVALBOARD_VIA_CONFIG_OFFSET,
        SVALBOARD_VIA_CONFIG_SIZE,
        DYNAMIC_KEYMAP_LAYER_COUNT,
        SVALBOARD_SAVED_VALUES_SCHEMA,
    };
    uint32_t stamp = layout_stamp(values, sizeof(values) / sizeof(values[0]));
    magic[0]       = 0xA5;
    magic[1]       = 0x5B;
    magic[2]       = (stamp >> 24) & 0xFF;
    magic[3]       = (stamp >> 16) & 0xFF;
    magic[4]       = (stamp >> 8) & 0xFF;
    magic[5]       = stamp & 0xFF;
}

static bool svalboard_eeprom_is_valid(void) {
    uint8_t stored[SVALBOARD_MAGIC_SIZE];
    uint8_t expected[SVALBOARD_MAGIC_SIZE];
    via_read_custom_config(stored, SVALBOARD_MAGIC_OFFSET, SVALBOARD_MAGIC_SIZE);
    svalboard_get_magic(expected);
    return memcmp(stored, expected, SVALBOARD_MAGIC_SIZE) == 0;
}

void svalboard_eeprom_set_valid(void) {
    uint8_t magic[SVALBOARD_MAGIC_SIZE];
    svalboard_get_magic(magic);
    via_update_custom_config(magic, SVALBOARD_MAGIC_OFFSET, SVALBOARD_MAGIC_SIZE);
}

void write_eeprom_kb(void) {
    via_update_custom_config(&global_saved_values, SVALBOARD_VIA_CONFIG_OFFSET, SVALBOARD_VIA_CONFIG_SIZE);
}
#else
static bool svalboard_eeprom_is_valid(void) {
    return true;
}
void svalboard_eeprom_set_valid(void) {}
void write_eeprom_kb(void) {}
#endif

#define HSV(c) (struct layer_hsv){(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF}

// Factory defaults for every saved keyboard setting, in RAM only.
void svalboard_saved_values_defaults(void) {
    memset(&global_saved_values, 0, sizeof(global_saved_values));

    global_saved_values.right_dpi_index     = 3;
    global_saved_values.left_dpi_index      = 3;
    global_saved_values.mh_timer_index      = 3;
    global_saved_values.left_scroll         = true;
    global_saved_values.auto_mouse          = true;
    global_saved_values.axis_scroll_lock    = true;
    global_saved_values.turbo_scan          = 0;
    global_saved_values.natural_scroll      = false;
    global_saved_values.left_automouse      = true;
    global_saved_values.right_automouse     = true;
    global_saved_values.automouse_threshold = 15;
    global_saved_values.automouse_decay     = 7; // 70ms
    // Revision B settles more slowly; start it on explicit conservative timing.
    global_saved_values.scan_prewait_us     = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_PREWAIT_US : 0;
    global_saved_values.scan_postwait_us    = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_POSTWAIT_US : 0;
    global_saved_values.scan_period_us      = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US : 0;
    global_saved_values.scan_idle_period_ms = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS : 0;
    global_saved_values.scan_idle_after_ms  = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS : 0;
    global_saved_values.scan_deep_after_s   = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S : 0;
    global_saved_values.scan_deep_period_ms = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS : 0;
    global_saved_values.idle_flags          = SVAL_IDLE_FLAGS_DEFAULT;
    global_saved_values.scan_deep_clock_idx = 0;

    // Layer colors
    global_saved_values.layer_colors[0]  = HSV(0x55FFFF); // Green
    global_saved_values.layer_colors[1]  = HSV(0x15FFFF); // Orange
    global_saved_values.layer_colors[2]  = HSV(0x95FFFF); // Azure
    global_saved_values.layer_colors[3]  = HSV(0x0BB0FF); // Coral
    global_saved_values.layer_colors[4]  = HSV(0x2BFFFF); // Yellow
    global_saved_values.layer_colors[5]  = HSV(0x80FF80); // Teal
    global_saved_values.layer_colors[6]  = HSV(0x00FFFF); // Red
    global_saved_values.layer_colors[7]  = HSV(0x00FFFF); // Red
    global_saved_values.layer_colors[8]  = HSV(0xEAFFFF); // Pink
    global_saved_values.layer_colors[9]  = HSV(0xBFFF80); // Purple
    global_saved_values.layer_colors[10] = HSV(0x0BB0FF); // Coral
    global_saved_values.layer_colors[11] = HSV(0x6AFFFF); // Spring Green
    global_saved_values.layer_colors[12] = HSV(0x80FF80); // Teal
    global_saved_values.layer_colors[13] = HSV(0x80FFFF); // Turquoise
    global_saved_values.layer_colors[14] = HSV(0x2BFFFF); // Yellow
    global_saved_values.layer_colors[15] = HSV(0xD5FFFF); // Magenta
}

// The stored keycodes were written under a different keycode numbering than
// this firmware's: reset what the Sval module stores outside the VIA keymap too.
void via_keycodes_reset_kb(void) {
    sval_eeprom_invalidate();
}

// GET_INFO byte 18 (sval.c): bit 2, settings were reset because a different
// firmware build was installed; bit 1 also covers a build record that could
// not be saved, since the next boot then resets again.
uint8_t sval_storage_flags_kb(void) {
    return (build_reset_notice_pending() ? 4 : 0) | (build_reset_record_failed() ? 2 : 0);
}

void sval_storage_reset_clear_kb(void) {
    build_reset_notice_clear();
}

// Runs after the EEPROM driver is up and before anything validates or resets
// stored settings.
void keyboard_pre_init_kb(void) {
    identity_init();
    sval_thumb_hand_probe(); // before ws2812/rgblight and split init use GP19
#ifdef SVAL_EITHER_HAND
    sval_side_init(); // the side, before split_pre_init() asks and before the build reset formats the settings
#endif
    build_reset_pre_init(); // a different firmware build: reset stored settings
    keyboard_pre_init_user();
}

void read_eeprom_kb(void) {
    // Check if EEPROM data is valid (matches current firmware version)
    if (!svalboard_eeprom_is_valid()) {
        // Fresh EEPROM - apply defaults
        svalboard_saved_values_defaults();
        write_eeprom_kb();
        svalboard_eeprom_set_valid();
    } else {
#if VIA_ENABLE
        via_read_custom_config(&global_saved_values, SVALBOARD_VIA_CONFIG_OFFSET, SVALBOARD_VIA_CONFIG_SIZE);
#endif
    }
    sval_active_layer = 0;
}

static const char YES[] = "yes";
static const char NO[]  = "no";

const char *yes_or_no(int flag) {
    if (flag) {
        return YES;
    } else {
        return NO;
    }
}

const uint16_t dpi_choices[] = {200, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 12000}; // If we need more, add them.
#define DPI_CHOICES_LENGTH (sizeof(dpi_choices) / sizeof(dpi_choices[0]))
extern bool is_mac;

// Boost state lives in keymaps/keymap_support.c; surfaced here for output_keyboard_info.
#include "axis_scale.h"
extern uint8_t      boost_hold_2, boost_hold_3, boost_hold_5;
extern bool         boost_toggle_2, boost_toggle_3, boost_toggle_5;
extern axis_scale_t boost_x;

void output_keyboard_info(void) {
    char output_buffer[256];

    sprintf(output_buffer, "%s:%s @ %s\n", QMK_KEYBOARD, QMK_KEYMAP, QMK_VERSION);
    send_string(output_buffer);
    if (sval_storage_was_reset()) send_string("Settings were reset: storage could not be read. Reload your layout file.\n");
    if (sval_storage_write_failed()) send_string("Settings can't be saved right now: changes will be lost on restart. Unplug and reconnect, then check again.\n");
    sprintf(output_buffer, "Left Ptr: Scroll %s, cpi: %d, Right Ptr: Scroll %s, cpi: %d\n", yes_or_no(global_saved_values.left_scroll), dpi_choices[global_saved_values.left_dpi_index], yes_or_no(global_saved_values.right_scroll), dpi_choices[global_saved_values.right_dpi_index]);
    send_string(output_buffer);
    sprintf(output_buffer, "Axis Scroll Lock: %s (is Mac: %d), Natural Scroll: %s, Mouse Layer: %s (left: %s, right: %s), Mouse Layer Timeout: %d, Turbo Scan: %d\n", yes_or_no(global_saved_values.axis_scroll_lock), is_mac, yes_or_no(global_saved_values.natural_scroll), yes_or_no(global_saved_values.auto_mouse), yes_or_no(global_saved_values.left_automouse), yes_or_no(global_saved_values.right_automouse), mh_timer_choices[global_saved_values.mh_timer_index], global_saved_values.turbo_scan);
    send_string(output_buffer);
    sprintf(output_buffer, "BOOST: hold[2,3,5]=%d,%d,%d  tg[2,3,5]=%d,%d,%d  mult=%d\n", boost_hold_2, boost_hold_3, boost_hold_5, boost_toggle_2, boost_toggle_3, boost_toggle_5, boost_x.mult);
    send_string(output_buffer);
    uint16_t pre, post;
    sval_scan_timing(&pre, &post);
    sprintf(output_buffer, "HW rev: %s, Scan timing: pre %d us, post %d us (%s)\n", sval_hw_rev_is_flipfet() ? "B (flipfet)" : "A", pre, post, (global_saved_values.scan_prewait_us || global_saved_values.scan_postwait_us) ? "explicit" : "turbo table");
    send_string(output_buffer);
    uint32_t frame_us;
    uint16_t led_us;
    uint8_t  stage;
    sval_scan_stats(&frame_us, &led_us, &stage);
    sprintf(output_buffer, "Scan period: %d us; light idle %d ms after %d ms; deep idle %d ms after %d s; measured frame %lu us, LED on %d us, stage %d\n", global_saved_values.scan_period_us, global_saved_values.scan_idle_period_ms, global_saved_values.scan_idle_after_ms, global_saved_values.scan_deep_period_ms, global_saved_values.scan_deep_after_s, (unsigned long)frame_us, led_us, stage);
    send_string(output_buffer);
    sprintf(output_buffer, "Idle power: pointer rest %s, RGB dim %s, CPU sleep %s, low clock in deep idle %s (%d MHz), long naps %s; clock now %d MHz\n", (global_saved_values.idle_flags & SVAL_IDLE_POINTER_REST) ? "on" : "off", (global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM) ? "on" : "off", (global_saved_values.idle_flags & SVAL_IDLE_CPU_SLEEP) ? "on" : "off", (global_saved_values.idle_flags & SVAL_IDLE_LOW_CLOCK) ? "on" : "off", sval_deep_clock_mhz(), (global_saved_values.idle_flags & SVAL_IDLE_LONG_NAP) ? "on" : "off", sval_clock_mhz());
    send_string(output_buffer);
}

const uint16_t sval_postwait_us[] = {90, 60, 45, 30, 25, 20, 15};
const uint16_t sval_prewait_us[]  = {90, 60, 45, 30, 25, 20, 15};
#define TURBO_CHOICES_LENGTH (sizeof(sval_postwait_us) / sizeof(sval_postwait_us[0]))
void change_turbo_scan(void) {
    if (global_saved_values.turbo_scan + 1 < TURBO_CHOICES_LENGTH) {
        global_saved_values.turbo_scan++;
    } else {
        global_saved_values.turbo_scan = 0;
    }
    write_eeprom_kb();
}

void increase_left_dpi(void) {
    if (global_saved_values.left_dpi_index + 1 < DPI_CHOICES_LENGTH) {
        global_saved_values.left_dpi_index++;
        set_left_dpi(global_saved_values.left_dpi_index);
        write_eeprom_kb();
    }
}

void decrease_left_dpi(void) {
    if (global_saved_values.left_dpi_index > 0) {
        global_saved_values.left_dpi_index--;
        set_left_dpi(global_saved_values.left_dpi_index);
        write_eeprom_kb();
    }
}

void increase_right_dpi(void) {
    if (global_saved_values.right_dpi_index + 1 < DPI_CHOICES_LENGTH) {
        global_saved_values.right_dpi_index++;
        set_right_dpi(global_saved_values.right_dpi_index);
        write_eeprom_kb();
    }
}

void decrease_right_dpi(void) {
    if (global_saved_values.right_dpi_index > 0) {
        global_saved_values.right_dpi_index--;
        set_right_dpi(global_saved_values.right_dpi_index);
        write_eeprom_kb();
    }
}

int16_t get_left_dpi() {
    return dpi_choices[global_saved_values.left_dpi_index];
}

int16_t get_right_dpi() {
    return dpi_choices[global_saved_values.right_dpi_index];
}

void set_left_dpi(uint8_t index) {
    uprintf("LDPI: %d %d\n", index, dpi_choices[index]);
    pointing_device_set_cpi_on_side(true, dpi_choices[index]);
}

void set_right_dpi(uint8_t index) {
    uprintf("RDPI: %d %d\n", index, dpi_choices[index]);
    pointing_device_set_cpi_on_side(false, dpi_choices[index]);
}

void set_dpi_from_eeprom(void) {
    set_left_dpi(global_saved_values.left_dpi_index);
    set_right_dpi(global_saved_values.right_dpi_index);
}

// ---- Host activity. Deep idle paces the whole main loop off the nap length, so a config
// app talking over raw HID would get one request/response round trip per nap (20 ms on the
// master). A bootstrap is hundreds of round trips, which is long enough that the host gives
// up before the board has said anything wrong. Every host packet stamps the clock here; while
// the stamp is fresh the pacing gate keeps full speed and 1 ms naps. The scan period itself
// is left alone, so the sensor LEDs stay in deep idle and the power saving is kept.
static uint32_t host_last_ms  = 0;
static bool     host_seen_any = false;

void sval_host_packet_kb(void) {
    host_last_ms  = timer_read32();
    host_seen_any = true;
}

uint32_t sval_host_idle_ms(void) {
    return host_seen_any ? timer_elapsed32(host_last_ms) : UINT32_MAX;
}

bool sval_host_recent(void) {
    return host_seen_any && timer_elapsed32(host_last_ms) < SVAL_HOST_ACTIVE_MS;
}

// ---- RGB idle dimming (master). Light idle divides the brightness, deep idle turns the
// strip off; the first input restores it. Layer colour changes while dimmed keep using the
// awake brightness so the dimmed value never reaches EEPROM.
static uint8_t  rgb_idle_applied  = 0; // 0 awake, 1 dimmed, 2 off
static uint32_t rgb_last_write_ms = 0;

bool sval_rgb_idle_quiesced(void) {
    if (!is_keyboard_master()) return true;
    if (!(global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM)) return true;
    return rgb_idle_applied == 2 && timer_elapsed32(rgb_last_write_ms) > 5;
}
static uint8_t rgb_awake_val     = 0;
static uint8_t rgb_dim_val       = 0;
static bool    rgb_awake_enabled = false;

uint8_t sval_rgb_awake_val(void) {
    return rgb_idle_applied ? rgb_awake_val : rgblight_get_val();
}

static void sval_rgb_idle_restore(void) {
    if (!rgb_idle_applied) return;
    sval_clock_full(); // WS2812 timing needs the clock the driver was set up for
    if (rgb_awake_enabled && !rgblight_is_enabled()) rgblight_enable_noeeprom();
    // Only undo our own change; if someone adjusted the brightness while dimmed, keep theirs.
    if (rgb_idle_applied == 2 || rgblight_get_val() == rgb_dim_val) {
        rgblight_sethsv_noeeprom(rgblight_get_hue(), rgblight_get_sat(), rgb_awake_val);
    }
    rgb_idle_applied = 0;
}

void sval_rgb_idle_task(void) {
    if (!(global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM)) {
        sval_rgb_idle_restore();
        return;
    }
    uint32_t quiet = last_input_activity_elapsed();
    uint8_t  want  = 0;
    if (global_saved_values.scan_deep_after_s && quiet > (uint32_t)global_saved_values.scan_deep_after_s * 1000u) {
        want = 2;
    } else if (global_saved_values.scan_idle_after_ms && quiet > global_saved_values.scan_idle_after_ms) {
        want = 1;
    }
    if (want == rgb_idle_applied) return;
    if (want == 0) {
        sval_rgb_idle_restore();
        return;
    }
    if (rgb_idle_applied == 0) {
        rgb_awake_val     = rgblight_get_val();
        rgb_awake_enabled = rgblight_is_enabled();
    }
    if (want == 1) {
        rgb_dim_val = rgb_awake_val / SVAL_IDLE_RGB_LIGHT_DIV;
        if (rgb_awake_val && rgb_dim_val < 2) rgb_dim_val = 2;
        rgblight_sethsv_noeeprom(rgblight_get_hue(), rgblight_get_sat(), rgb_dim_val);
    } else {
        rgblight_disable_noeeprom();
    }
    rgb_last_write_ms = timer_read32();
    rgb_idle_applied  = want;
}

__attribute__((weak)) void sval_pointer_rest_apply(void) {}
__attribute__((weak)) void sval_pointer_status(sval_idle_status_t *st) {
    st->sensor_present = 0;
}
__attribute__((weak)) void    sval_pointer_identity(sval_pointer_identity_t *id) {}
__attribute__((weak)) uint8_t sval_pointer_own_bus(void) {
    return 0;
}
// Starts the driver for what the probe found; a build with every driver in uses it (pointing_auto).
__attribute__((weak)) void sval_pointer_select(const sval_probe_result_t *found) {}

__attribute__((weak)) uint8_t sval_pointer_retry_own(void) {
    return SVAL_RETRY_NONE;
}

// While this half has no working pointing module, it looks again for 30 s (pointer_reprobe.c).
#define SVAL_POINTER_PROBE_AFTER_MS SVAL_REPROBE_FIRST_MS

// This half's side and pointing module.
static void sval_local_pointer_identity(sval_pointer_identity_t *id) {
    memset(id, 0, sizeof(*id));
    sval_pointer_identity(id);
    // The other buses found something (or nothing) where the build's own module didn't answer;
    // a later search's find replaces it.
    bool own_missing = id->built_for == SVAL_SENSOR_NONE || id->detected == SVAL_SENSOR_NO_RESPONSE;
    const sval_probe_result_t *found = sval_pointer_found();
    if (sval_pointer_searched() && own_missing && (found->detected != SVAL_SENSOR_NO_RESPONSE || id->built_for == SVAL_SENSOR_NONE)) {
        id->detected           = found->detected;
        id->product_id         = found->id0;
        id->inverse_product_id = found->id1;
    }
    id->flags |= SVAL_POINTER_REPORTED | (is_keyboard_left() ? SVAL_POINTER_LEFT : 0);
#ifdef SVAL_EITHER_HAND
    if (!sval_side_known()) id->flags = (id->flags & ~SVAL_POINTER_LEFT) | SVAL_POINTER_SIDE_UNKNOWN;
#endif
}

void sval_idle_status(sval_idle_status_t *st) {
    memset(st, 0, sizeof(*st));
    sval_pointer_status(st);
    st->rgb_val_now   = rgblight_get_val();
    st->rgb_val_awake = sval_rgb_awake_val();
    st->rgb_stage     = rgb_idle_applied;
    st->rgb_enabled   = rgblight_is_enabled();
}

void sval_set_active_layer(uint32_t layer, bool save) {
    if (layer > 15) layer = 15;
    sval_active_layer = layer;
    // Layer callbacks first run from quantum_init(), before rgblight_init(). Right
    // after settings are initialized the RGB defaults are already enabled in RAM,
    // and driving the uninitialized LEDs blocks the first boot for good.
    // keyboard_post_init_kb() applies the colour once lighting is up.
    if (!is_rgblight_initialized) return;
    struct layer_hsv cols  = global_saved_values.layer_colors[layer];
    uint8_t          awake = sval_rgb_awake_val();
    uint8_t          shown = rgb_idle_applied == 1 ? rgb_dim_val : awake;
    if (save) {
        rgblight_sethsv(cols.hue, cols.sat, awake); // store with the awake brightness
        if (shown != awake) rgblight_sethsv_noeeprom(cols.hue, cols.sat, shown);
    } else {
        rgblight_sethsv_noeeprom(cols.hue, cols.sat, shown);
    }
}

// RPC listener for split keyboard sync
void kb_sync_listener(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    const presence_rpc_t *in                = (const presence_rpc_t *)in_data;
    global_saved_values.turbo_scan          = in->turbo_scan;
    global_saved_values.scan_prewait_us     = in->scan_prewait_us;
    global_saved_values.scan_postwait_us    = in->scan_postwait_us;
    global_saved_values.scan_period_us      = in->scan_period_us;
    global_saved_values.scan_idle_period_ms = in->scan_idle_period_ms;
    global_saved_values.scan_idle_after_ms  = in->scan_idle_after_ms;
    global_saved_values.scan_deep_after_s   = in->scan_deep_after_s;
    global_saved_values.scan_deep_period_ms = in->scan_deep_period_ms;
    if (global_saved_values.idle_flags != in->idle_flags) {
        global_saved_values.idle_flags = in->idle_flags;
        sval_pointer_rest_apply();
    }
    global_saved_values.scan_deep_clock_idx = in->scan_deep_clock_idx;
#ifdef SVAL_UPDATER
    // The master's updater light (D24), when its ping carries it.
    bool has_light = in_buflen >= sizeof(presence_rpc_t);
    updater_split_light_seen(has_light, has_light ? in->update_light : 0);
#endif
    // Tell the master this half's side and sensor (Sval 0x2C). Older masters ignore it.
    if (out_data && out_buflen >= sizeof(presence_rsp_t)) {
        presence_rsp_t *out = (presence_rsp_t *)out_data;
        out->magic[0]       = SVAL_PRESENCE_RSP_MAGIC0;
        out->magic[1]       = SVAL_PRESENCE_RSP_MAGIC1;
        sval_local_pointer_identity(&out->pointer);
#ifdef SVAL_UPDATER
        // And this build's version, for the master's mismatch check (D17, V).
        update_split_presence_fill(out->update, sizeof(out->update));
#endif
    }
}

// Scan Lab requests from the master run here on the other half.
static void scanlab_rpc_listener(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
#ifdef SVAL_AZOTEQ_MULTITOUCH
    // The native touchpad's requests for this half's pad share this RPC
    // (azoteq/multitouch.h): the split message table stays the same.
    if (sval_mt_rpc(in_buflen, (const uint8_t *)in_data, out_buflen, (uint8_t *)out_data)) return;
#endif
#ifdef SVAL_PERF_STATS
    if (sval_perf_split_rpc(in_buflen, (const uint8_t *)in_data, out_buflen, (uint8_t *)out_data)) return;
#endif
#ifdef SVAL_EITHER_HAND
    if (sval_side_rpc(in_buflen, (const uint8_t *)in_data, out_buflen, (uint8_t *)out_data)) return; // Sval 0x33 for this half
#endif
    uint8_t req[SCANLAB_REQ_LEN] = {0};
    uint8_t rsp[SCANLAB_RSP_LEN] = {0};
    memcpy(req, in_data, in_buflen < SCANLAB_REQ_LEN ? in_buflen : SCANLAB_REQ_LEN);
    scanlab_handle(req, rsp);
    memcpy(out_data, rsp, out_buflen < SCANLAB_RSP_LEN ? out_buflen : SCANLAB_RSP_LEN);
}

void keyboard_post_init_kb(void) {
    build_reset_post_init(); // the settings were reset in quantum_init()
#ifdef SVAL_AZOTEQ_MULTITOUCH
    sval_mt_init();
#endif
    read_eeprom_kb();
    sval_pointer_rest_apply(); // the sensor was initialised before the flags were read
    sval_sleep_gating_init();
    set_dpi_from_eeprom();
    keyboard_post_init_user();
    scanlab_init();
#ifdef SVAL_UPDATER
    update_split_init(); // before kb_sync_listener can answer a presence ping
#endif
    transaction_register_rpc(KEYBOARD_SYNC_A, kb_sync_listener);
    transaction_register_rpc(KEYBOARD_SYNC_B, scanlab_rpc_listener);
    if (is_keyboard_master()) {
        sval_set_active_layer(sval_active_layer, false);
    }
}

static bool is_connected = false;
bool        sval_other_half_connected(void) {
    return is_connected;
}

// The other half's pointer identity from its last ping reply (zero = not reported).
static sval_pointer_identity_t other_pointer;

// Sval POINTING_HARDWARE (0x2C) reply, after [0xDF] [0x2C]:
// [format = 1] [this half: 5 bytes] [other half: 5 bytes], each a sval_pointer_identity_t.
// The other half's block stays zero while it is disconnected or runs firmware without this.
void sval_pointing_hardware_kb(uint8_t *out, uint8_t length) {
    if (length < 1 + 2 * sizeof(sval_pointer_identity_t)) return;
    sval_pointer_identity_t mine;
    sval_local_pointer_identity(&mine);
    out[0] = 1;
    memcpy(&out[1], &mine, sizeof(mine));
    if (is_connected) memcpy(&out[1 + sizeof(mine)], &other_pointer, sizeof(other_pointer));
#ifdef SVAL_AZOTEQ_MULTITOUCH
    // The OS drives these pads as a touchpad: the pointer settings don't apply.
    for (uint8_t b = 0; b < 2; b++) {
        sval_pointer_identity_t *id = (sval_pointer_identity_t *)&out[1 + b * sizeof(mine)];
        if ((id->flags & SVAL_POINTER_REPORTED) && sval_mt_os_touchpad((id->flags & SVAL_POINTER_LEFT) ? 0 : 1)) id->flags |= SVAL_POINTER_OS_TOUCHPAD;
    }
#endif
}

// The identity block for one hand: this half's, or the other half's while it is connected.
static inline bool sval_pointer_for_hand(bool left, sval_pointer_identity_t *id, bool *local) {
    sval_local_pointer_identity(id);
    *local = true;
    if (((id->flags & SVAL_POINTER_LEFT) != 0) == left) return true;
    *local = false;
    if (is_connected && (other_pointer.flags & SVAL_POINTER_REPORTED) && ((other_pointer.flags & SVAL_POINTER_LEFT) != 0) == left) {
        *id = other_pointer;
        return true;
    }
    return false;
}

// Hands whose pointing module is an Azoteq pad (bit 0 left, bit 1 right), for
// the native touchpad (azoteq/multitouch.c) and Velocity Scroll.
uint8_t sval_pad_hands(void) {
    uint8_t mask = 0;
    for (uint8_t hand = 0; hand < 2; hand++) {
        sval_pointer_identity_t id;
        bool                    local;
        if (sval_pointer_for_hand(hand == 0, &id, &local) && id.detected == SVAL_SENSOR_AZOTEQ) mask |= 1u << hand;
    }
    return mask;
}

#ifdef SVAL_FRAGMENT_IDX_LEFT_POINTING
// Each hand's pointing fragment (Sval 0x18) from the module identity 0x2C reports.
// 0xFF until that half has settled: not checked yet, or its own module missing before the
// boot probe of the other buses has run (the other half's probe runs on its own clock, so
// allow it a second more).
#    define SVAL_POINTER_SETTLED_MS (SVAL_POINTER_PROBE_AFTER_MS + 1000)
uint8_t sval_fragment_detect(uint8_t instance_idx) {
    bool left;
    if (instance_idx == SVAL_FRAGMENT_IDX_LEFT_POINTING)
        left = true;
    else if (instance_idx == SVAL_FRAGMENT_IDX_RIGHT_POINTING)
        left = false;
    else
        return SVAL_FRAGMENT_ID_NONE;
    sval_pointer_identity_t id;
    bool                    local;
    if (!sval_pointer_for_hand(left, &id, &local)) return SVAL_FRAGMENT_ID_NONE;
    if (id.detected == SVAL_SENSOR_NO_RESPONSE && (local ? !sval_pointer_searched() : timer_read32() < SVAL_POINTER_SETTLED_MS)) return SVAL_FRAGMENT_ID_NONE;
    return sval_pointing_fragment_for_detected(id.detected);
}
#endif

__attribute__((weak)) void sval_pointing_modes(sval_pointing_modes_t *m) {
    memset(m, 0, sizeof(*m));
}

#if defined(POINTING_DEVICE_ENABLE) && defined(SPLIT_POINTING_ENABLE) && defined(POINTING_DEVICE_COMBINED)
// Motion since the last POINTING_MOTION (0x2D) read, per physical hand ([0] left).
static sval_motion_acc_t motion_acc[2];
static uint32_t          motion_last_read;
static bool              motion_read_once = false;

// Both halves' reports after rotation and invert, before sniper, boost and scroll
// conversion (keymap_support.c). Counted only while a host app is reading them.
report_mouse_t pointing_device_task_combined_kb(report_mouse_t left_report, report_mouse_t right_report) {
#if defined(SVAL_AZOTEQ_MULTITOUCH) && defined(MOUSE_EXTENDED_REPORT)
    // The other half's pad news counter rides in boot_x (azoteq.c); take it
    // off before anything else sees the report.
    sval_mt_remote_hint((uint8_t)(is_keyboard_left() ? right_report.boot_x : left_report.boot_x));
    left_report.boot_x = right_report.boot_x = 0;
#endif
    if (motion_read_once && timer_elapsed32(motion_last_read) < SVAL_HOST_ACTIVE_MS) {
        sval_motion_accumulate(&motion_acc[0], left_report.x, left_report.y, left_report.h, left_report.v, left_report.buttons);
        sval_motion_accumulate(&motion_acc[1], right_report.x, right_report.y, right_report.h, right_report.v, right_report.buttons);
    }
#ifdef SVAL_AZOTEQ_MULTITOUCH
    // While the native touchpad has the pads, their mouse output is zero; a
    // hand with a trackball or TrackPoint is never touched.
    if (sval_mt_suppress_hand(0)) left_report = (report_mouse_t){0};
    if (sval_mt_suppress_hand(1)) right_report = (report_mouse_t){0};
#endif
#ifdef SVAL_EITHER_HAND
    // As keys (side.c): nothing from a hand whose side is not sound.
    if (!sval_side_row_allowed(0)) left_report = (report_mouse_t){0};
    if (!sval_side_row_allowed(MATRIX_ROWS / 2)) right_report = (report_mouse_t){0};
#endif
    return pointing_device_task_combined_user(left_report, right_report);
}

void sval_pointing_motion_kb(uint8_t *out, uint8_t length) {
    if (length < SVAL_MOTION_REPORT_LEN) return;
    sval_pointing_modes_t modes;
    sval_pointing_modes(&modes);
    out[0]           = SVAL_MOTION_FORMAT;
    out[1]           = modes.global_bits & 0x3F;
    out[2]           = sval_motion_elapsed(!motion_read_once, timer_elapsed32(motion_last_read));
    motion_last_read = timer_read32();
    motion_read_once = true;
    for (uint8_t hand = 0; hand < 2; hand++) {
        bool                    left = hand == 0;
        sval_pointer_identity_t id;
        bool                    local;
        if (!sval_pointer_for_hand(left, &id, &local) || !(id.flags & SVAL_POINTER_INIT_OK)) continue; // block stays zero
        uint8_t flags = SVAL_MOTION_VALID;
        if (left ? modes.left_scroll : modes.right_scroll) flags |= SVAL_MOTION_SCROLLING;
        if (left ? modes.left_automouse : modes.right_automouse) flags |= SVAL_MOTION_AUTOMOUSE;
        sval_motion_pack_hand(&out[left ? SVAL_MOTION_LEFT_OFF : SVAL_MOTION_RIGHT_OFF], &motion_acc[hand], flags);
    }
    out[SVAL_MOTION_AXIS_OFF] = modes.axis_state;
    if (modes.has_vs && length >= SVAL_MOTION_VS_OFF + SVAL_MOTION_VS_LEN) sval_motion_pack_vs(&out[SVAL_MOTION_VS_OFF], &modes.vs);
    memset(motion_acc, 0, sizeof(motion_acc));
}
#endif

#ifdef SVAL_NATIVE_PTP
// POINTING_CONTACTS (Sval 0x2F, docs/protocol.md): every touch contact on its
// pad, from the native touchpad. Only the USB half has it.
void sval_pointing_contacts_kb(uint8_t *out, uint8_t length) {
    if (!is_keyboard_master()) return;
    sval_ptp_contacts(out, length);
}
#endif

// Custom USB wake handler for split keyboards with NO_USB_STARTUP_CHECK
// This replaces the wake functionality that NO_USB_STARTUP_CHECK disables
static void sval_usb_wake_handler(void) {
    // Only master half handles USB wake
    if (!is_keyboard_master()) return;

    // Check if USB is in suspended state
    if (USB_DRIVER.state == USB_SUSPENDED) {
        // Check if host has enabled remote wakeup capability
        if (USB_DRIVER.status & USB_GETSTATUS_REMOTE_WAKEUP_ENABLED) {
            // Check for wake condition (key pressed)
            if (suspend_wakeup_condition()) {
                usbWakeupHost(&USB_DRIVER);
#if USB_SUSPEND_WAKEUP_DELAY > 0
                wait_ms(USB_SUSPEND_WAKEUP_DELAY);
#endif
            }
        }
    }
}

#if defined(SVAL_TOUCH_DEV) || defined(SVAL_PERF_STATS)
// Raw HID 0xFC (modules/svalboard/core/client_wrapper.c): the dev builds'
// touchpad command (op 0x42) and timing counters (op 0x43). Not a released
// protocol; release builds have neither.
void sval_dev_command(uint8_t *data, uint8_t length) {
    if (length != RAW_EPSIZE) return;
    uint8_t op = data[6];
#    ifdef SVAL_PERF_STATS
    if (op == 0x43) {
        sval_perf_command(data, length);
        return;
    }
#    endif
#    ifdef SVAL_TOUCH_DEV
    if (op == 0x42) {
        sval_mt_command(data, length);
        return;
    }
#    endif
    memset(data + 6, 0, length - 6);
    data[6] = 0x80 | op;
    data[7] = 2; // unknown op
}
#endif

void housekeeping_task_kb(void) {
#ifdef SVAL_PERF_STATS
    sval_perf_loop();
#endif
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    ifdef SVAL_PERF_STATS
    uint32_t mt_started = sval_perf_now_us();
    sval_mt_task();
    sval_perf_section(SVAL_PERF_SEC_TOUCH_TASK, sval_perf_now_us() - mt_started);
#    else
    sval_mt_task();
#    endif
#endif
    sval_pointer_probe_task();
#ifdef SVAL_EITHER_HAND
    sval_side_task();
#endif
    sval_usb_wake_handler();
    scanlab_housekeeping();
#ifdef SVAL_KEYTEST
    keytest_task();
#endif
#ifdef SVAL_UPDATER
    // The updater owns the LEDs and the flash while active: no idle dimming,
    // and no identity reboot. On the half without USB, the session the other
    // half runs over KEYBOARD_UPDATE does its slow work here first (M2).
    if (!is_keyboard_master()) update_split_slave_task();
    updater_task();
    if (!updater_active()) identity_task();
#else
    identity_task();
#endif

    if (is_keyboard_master()) {
#ifdef SVAL_UPDATER
        if (!updater_active())
#endif
            sval_rgb_idle_task();
        static uint32_t last_ping = 0;
#ifdef SVAL_UPDATER
        // No presence ping while the split link is paused (D15); the first
        // pass after the resume pings.
        if (timer_elapsed(last_ping) > 500 && !sval_split_paused()) {
#else
        if (timer_elapsed(last_ping) > 500) {
#endif
            presence_rpc_t rpcout = {global_saved_values.turbo_scan, global_saved_values.scan_prewait_us, global_saved_values.scan_postwait_us, global_saved_values.scan_period_us, global_saved_values.scan_idle_period_ms, global_saved_values.scan_idle_after_ms, global_saved_values.scan_deep_after_s, global_saved_values.scan_deep_period_ms, global_saved_values.idle_flags, global_saved_values.scan_deep_clock_idx};
#ifdef SVAL_UPDATER
            rpcout.update_light = updater_split_light();
#endif
            presence_rsp_t rpcin  = {0};
            if (transaction_rpc_exec(KEYBOARD_SYNC_A, sizeof(presence_rpc_t), &rpcout, sizeof(presence_rsp_t), &rpcin)) {
                if (rpcin.magic[0] == SVAL_PRESENCE_RSP_MAGIC0 && rpcin.magic[1] == SVAL_PRESENCE_RSP_MAGIC1 && (rpcin.pointer.flags & SVAL_POINTER_REPORTED)) {
                    other_pointer = rpcin.pointer;
                } else {
                    memset(&other_pointer, 0, sizeof(other_pointer));
                }
#ifdef SVAL_EITHER_HAND
                {
                    const uint8_t f = other_pointer.flags;
                    sval_side_other_set(true, (f & SVAL_POINTER_REPORTED) != 0, (f & SVAL_POINTER_SIDE_UNKNOWN) ? SVAL_SIDE_UNKNOWN : (f & SVAL_POINTER_LEFT) ? SVAL_SIDE_LEFT : SVAL_SIDE_RIGHT);
                }
#endif
#ifdef SVAL_UPDATER
                // The other half's update_presence_t, after the pointer block.
                update_split_presence_result(true, rpcin.update, sizeof(rpcin.update));
#endif
                if (!is_connected) {
                    is_connected = true;
#ifdef SVAL_UPDATER
                    // D21: no settings write while an update runs (the other
                    // half reconnects after its commit, while this one may be
                    // staging its own update).
                    if (!updater_active())
#endif
                        sval_on_reconnect();
                }
            } else {
#ifdef SVAL_UPDATER
                update_split_presence_result(false, NULL, 0);
#endif
#ifdef SVAL_EITHER_HAND
                sval_side_other_set(false, false, SVAL_SIDE_UNKNOWN);
#endif
                is_connected = false;
            }
            last_ping = timer_read32();
        }
    }
}

void sval_on_reconnect(void) {
    // Reset colors, or it won't communicate the right color.
    rgblight_sethsv_noeeprom(0, 0, rgblight_get_val()); // reuse existing val, so brightness doesn't reset
    sval_set_active_layer(sval_active_layer, true);
}

#ifndef SVALBOARD_REENABLE_BOOTMAGIC_LITE
// This is to override `bootmagic_lite` feature (see docs/feature_bootmagic.md),
// which can't be turned off in the usual way (via info.json) because setting
// `VIA_ENABLE` forces `BOOTMAGIC_ENABLE` in `builddefs/common_features.mk`.
//
// Obviously if you find this feature useful, you might want to define the
// SVALBOARD_... gating macro, and then possibly also (re-)define the
// `"bootmagic": { "matrix": [X,Y] },` in `info.json` to point the matrix at
// a more useful key than the [0,0] default. Ideally a center key, which is
// normally ~always present. Because the default (thumb knuckle) means that
// if you boot with the key pulled out, the eeprom gets cleared.

void bootmagic_lite(void) {
    // boo!
}
#endif

__attribute__((weak)) void recalibrate_pointer(void) {}

const char chordal_hold_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM = LAYOUT('R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'R', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', '*', '*', '*', '*', '*', '*', '*', '*', '*', '*', '*', '*');

#if VIA_ENABLE
// VIA custom keyboard value IDs (channel 1)
enum sval_via_value_id {
    id_left_dpi            = 0,
    id_left_scroll         = 1,
    id_right_dpi           = 2,
    id_right_scroll        = 3,
    id_automouse_enable    = 4,
    id_automouse_timeout   = 5,
    id_automouse_threshold = 6,
    id_natural_scroll      = 7,
    id_axis_lock           = 8,
    id_turbo_scan          = 9,
    id_automouse_decay     = 10, // Accumulator decay time in 10ms units
    id_left_automouse      = 11, // Left pointer movement activates the mouse layer
    id_right_automouse     = 12, // Right pointer movement activates the mouse layer
    id_scan_prewait_us     = 13, // u16: row-on settle time (0 = turbo table)
    id_scan_postwait_us    = 14, // u16: row-off recovery time (0 = turbo table)
    id_hw_revision         = 15, // read-only: 0 = revision A, 1 = revision B (flipfet)
    // 16-19: tapping settings
    id_scan_period_us      = 20, // u16: frame period while active (0 = unpaced)
    id_scan_idle_period_ms = 21, // u16: light idle frame period, ms (<= active = no change)
    id_scan_idle_after_ms  = 22, // u16: light idle timeout since last matrix change, ms (0 = never)
    id_scan_deep_after_s   = 23, // u16: deep idle timeout, seconds (0 = never)
    id_scan_deep_period_ms = 24, // u16: deep idle frame period, ms
    id_idle_pointer_rest   = 25, // toggle: trackball sensor rest modes
    id_idle_rgb_dim        = 26, // toggle: dim RGB in light idle, off in deep idle
    id_idle_cpu_sleep      = 27, // toggle: sleep the core between paced frames
    id_idle_low_clock      = 28, // toggle: 48 MHz system clock in deep idle
    id_idle_long_nap       = 29, // toggle: long naps in deep idle
    id_scan_deep_clock_idx = 30, // u8 dropdown: deep-idle clock 0 = 48 MHz, 1 = 24 MHz, 2 = 12 MHz
    id_thumb_hand          = 31, // read-only: [0] class (enum sval_thumb_hand), [1] raw bits (sval_thumb_hand_raw)
    id_tapping_term        = 16,
    id_permissive_hold     = 17,
    id_hold_on_other_key   = 18,
    id_retro_tapping       = 19,
    // 31: id_thumb_hand (above)
    id_layer0_color = 32,
    // 32-47 are layer colors (id_layer0_color + layer)
};

void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    if (length >= 3 && data[1] == KEYTEST_CHANNEL) {
#    ifdef SVAL_KEYTEST
        keytest_command(data, length);
#    else
        data[0] = id_unhandled;
#    endif
        return;
    }
    // New VIA API: data[0]=command, data[1]=channel, data[2]=value_id, data[3+]=value_data
    if (data[1] == SCANLAB_CHANNEL) {
        scanlab_via_command(data, length);
        return;
    }
    if (data[1] == IDENTITY_CHANNEL) {
        identity_via_command(data, length);
        return;
    }
#    ifdef SVAL_UPDATER
    if (data[1] == UPDATE_CHANNEL) {
        updater_via_command(data, length);
        return;
    }
#    endif
    uint8_t  command    = data[0];
    uint8_t *value_id   = &data[2];
    uint8_t *value_data = &data[3];

    switch (command) {
        case id_custom_set_value:
#    ifdef SVAL_TEST_HOOKS
            if (*value_id == 0xF2) { // test: pretend the keymap was written by an older keycode version
                nvm_via_update_keycodes_version(value_data[0]);
                break;
            }
#    endif
            switch (*value_id) {
                case id_left_dpi:
                    if (value_data[0] < DPI_CHOICES_LENGTH) {
                        global_saved_values.left_dpi_index = value_data[0];
                        set_left_dpi(value_data[0]);
                    }
                    break;
                case id_left_scroll:
                    global_saved_values.left_scroll = value_data[0];
                    break;
                case id_right_dpi:
                    if (value_data[0] < DPI_CHOICES_LENGTH) {
                        global_saved_values.right_dpi_index = value_data[0];
                        set_right_dpi(value_data[0]);
                    }
                    break;
                case id_right_scroll:
                    global_saved_values.right_scroll = value_data[0];
                    break;
                case id_automouse_enable:
                    global_saved_values.auto_mouse = value_data[0];
                    break;
                case id_automouse_timeout:
                    if (value_data[0] < 6) {
                        global_saved_values.mh_timer_index = value_data[0];
                    }
                    break;
                case id_natural_scroll:
                    global_saved_values.natural_scroll = value_data[0];
                    break;
                case id_axis_lock:
                    global_saved_values.axis_scroll_lock = value_data[0];
                    break;
                case id_turbo_scan:
                    if (value_data[0] < TURBO_CHOICES_LENGTH) {
                        global_saved_values.turbo_scan = value_data[0];
                    }
                    break;
                case id_automouse_threshold:
                    global_saved_values.automouse_threshold = value_data[0] | (value_data[1] << 8);
                    break;
                case id_automouse_decay:
                    global_saved_values.automouse_decay = value_data[0];
                    break;
                case id_left_automouse:
                    global_saved_values.left_automouse = value_data[0];
                    break;
                case id_right_automouse:
                    global_saved_values.right_automouse = value_data[0];
                    break;
                case id_scan_prewait_us:
                    global_saved_values.scan_prewait_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_postwait_us:
                    global_saved_values.scan_postwait_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_period_us:
                    global_saved_values.scan_period_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_idle_period_ms:
                    global_saved_values.scan_idle_period_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_idle_after_ms:
                    global_saved_values.scan_idle_after_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_after_s:
                    global_saved_values.scan_deep_after_s = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_period_ms:
                    global_saved_values.scan_deep_period_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_clock_idx:
                    global_saved_values.scan_deep_clock_idx = value_data[0] > 2 ? 2 : value_data[0];
                    break;
                case id_idle_pointer_rest:
                case id_idle_rgb_dim:
                case id_idle_cpu_sleep:
                case id_idle_low_clock:
                case id_idle_long_nap: {
                    uint8_t bit = 1u << (*value_id - id_idle_pointer_rest);
                    if (value_data[0])
                        global_saved_values.idle_flags |= bit;
                    else
                        global_saved_values.idle_flags &= ~bit;
                    if (bit == SVAL_IDLE_POINTER_REST) sval_pointer_rest_apply();
                    break;
                }
                default:
                    // Layer colors: id 32-47
                    if (*value_id >= id_layer0_color && *value_id < id_layer0_color + 16) {
                        uint8_t layer = *value_id - id_layer0_color;
                        // VIA color is H, S (2 bytes)
                        global_saved_values.layer_colors[layer].hue = value_data[0];
                        global_saved_values.layer_colors[layer].sat = value_data[1];
                        // Update display if this is the active layer
                        if (layer == sval_active_layer) {
                            sval_set_active_layer(layer, false);
                        }
                    }
                    break;
            }
            break;

        case id_custom_get_value:
#    ifdef SVAL_TEST_HOOKS
            if (*value_id == 0xF2) { // test: stored keycode version
                value_data[0] = nvm_via_read_keycodes_version();
                break;
            }
#    endif
            switch (*value_id) {
                case id_left_dpi:
                    value_data[0] = global_saved_values.left_dpi_index;
                    break;
                case id_left_scroll:
                    value_data[0] = global_saved_values.left_scroll;
                    break;
                case id_right_dpi:
                    value_data[0] = global_saved_values.right_dpi_index;
                    break;
                case id_right_scroll:
                    value_data[0] = global_saved_values.right_scroll;
                    break;
                case id_automouse_enable:
                    value_data[0] = global_saved_values.auto_mouse;
                    break;
                case id_automouse_timeout:
                    value_data[0] = global_saved_values.mh_timer_index;
                    break;
                case id_natural_scroll:
                    value_data[0] = global_saved_values.natural_scroll;
                    break;
                case id_axis_lock:
                    value_data[0] = global_saved_values.axis_scroll_lock;
                    break;
                case id_turbo_scan:
                    value_data[0] = global_saved_values.turbo_scan;
                    break;
                case id_automouse_threshold:
                    value_data[0] = global_saved_values.automouse_threshold & 0xFF;
                    value_data[1] = (global_saved_values.automouse_threshold >> 8) & 0xFF;
                    break;
                case id_automouse_decay:
                    value_data[0] = global_saved_values.automouse_decay;
                    break;
                case id_left_automouse:
                    value_data[0] = global_saved_values.left_automouse;
                    break;
                case id_right_automouse:
                    value_data[0] = global_saved_values.right_automouse;
                    break;
                case id_scan_prewait_us:
                    value_data[0] = global_saved_values.scan_prewait_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_prewait_us >> 8) & 0xFF;
                    break;
                case id_scan_postwait_us:
                    value_data[0] = global_saved_values.scan_postwait_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_postwait_us >> 8) & 0xFF;
                    break;
                case id_hw_revision:
                    value_data[0] = sval_hw_rev();
                    break;
                case id_thumb_hand:
                    value_data[0] = sval_thumb_hand();
                    value_data[1] = sval_thumb_hand_raw();
                    break;
                case id_scan_period_us:
                    value_data[0] = global_saved_values.scan_period_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_period_us >> 8) & 0xFF;
                    break;
                case id_scan_idle_period_ms:
                    value_data[0] = global_saved_values.scan_idle_period_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_idle_period_ms >> 8) & 0xFF;
                    break;
                case id_scan_idle_after_ms:
                    value_data[0] = global_saved_values.scan_idle_after_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_idle_after_ms >> 8) & 0xFF;
                    break;
                case id_scan_deep_after_s:
                    value_data[0] = global_saved_values.scan_deep_after_s & 0xFF;
                    value_data[1] = (global_saved_values.scan_deep_after_s >> 8) & 0xFF;
                    break;
                case id_scan_deep_period_ms:
                    value_data[0] = global_saved_values.scan_deep_period_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_deep_period_ms >> 8) & 0xFF;
                    break;
                case id_scan_deep_clock_idx:
                    value_data[0] = global_saved_values.scan_deep_clock_idx;
                    break;
                case id_idle_pointer_rest:
                case id_idle_rgb_dim:
                case id_idle_cpu_sleep:
                case id_idle_low_clock:
                case id_idle_long_nap:
                    value_data[0] = (global_saved_values.idle_flags >> (*value_id - id_idle_pointer_rest)) & 1;
                    break;
                default:
                    // Layer colors: id 32-47
                    if (*value_id >= id_layer0_color && *value_id < id_layer0_color + 16) {
                        uint8_t layer = *value_id - id_layer0_color;
                        value_data[0] = global_saved_values.layer_colors[layer].hue;
                        value_data[1] = global_saved_values.layer_colors[layer].sat;
                    }
                    break;
            }
            break;

        case id_custom_save:
            write_eeprom_kb();
            break;
    }
}
#endif // VIA_ENABLE
