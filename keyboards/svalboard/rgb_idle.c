// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// RGB while the board or the host rests (master): the idle dimming, and the
// RGBLIGHT_SLEEP blackout while the host sleeps. Host test:
// tests/sval_rgb_idle (util/updater_test/run.sh).

#include "quantum.h"
#include "svalboard.h"
#include "usb_main.h"
#if defined(VIA_ENABLE) && defined(RGBLIGHT_ENABLE)
#    include "via.h"
#endif

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
static bool     rgb_woken;
static uint32_t rgb_woken_at;

// Lights up as an input would, without one: the idle dimming counts from now.
void sval_rgb_wake(void) {
    rgb_woken    = true;
    rgb_woken_at = timer_read32();
}
static uint8_t rgb_dim_val       = 0;
static bool    rgb_awake_enabled = false;

uint8_t sval_rgb_awake_val(void) {
    return rgb_idle_applied ? rgb_awake_val : rgblight_get_val();
}

uint8_t sval_rgb_idle_stage(void) {
    return rgb_idle_applied;
}

uint8_t sval_rgb_shown_val(void) {
    return rgb_idle_applied == 1 ? rgb_dim_val : sval_rgb_awake_val();
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

// A key event, before any keycode runs (pre_process_record_kb): the lighting is
// awake again before a brightness, hue or toggle key steps it and saves it, so
// the dimmed value is never what they start from or store.
void sval_rgb_idle_input(void) {
    sval_rgb_idle_restore();
}

void sval_rgb_idle_task(void) {
    if (!(global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM)) {
        sval_rgb_idle_restore();
        return;
    }
    uint32_t quiet = last_input_activity_elapsed();
    if (rgb_woken && timer_elapsed32(rgb_woken_at) < quiet) quiet = timer_elapsed32(rgb_woken_at); // sval_rgb_wake()
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

#if defined(VIA_ENABLE) && defined(RGBLIGHT_ENABLE)
// VIA's lighting channel (quantum/via.c), with the idle dimming kept out of it:
// a set or a save works on the awake lighting, so the value saved is never the
// dimmed one, and a read reports the awake brightness, so a host that writes
// back what it read does not store the dimmed one either. The lighting dims
// again at the next idle step. Every other channel goes where via.c's default
// sends it; this build has no other QMK lighting or audio channel.
#    if defined(BACKLIGHT_ENABLE) || defined(RGB_MATRIX_ENABLE) || defined(LED_MATRIX_ENABLE) || defined(AUDIO_ENABLE)
#        error "via_custom_value_command: route the other QMK channels as quantum/via.c does"
#    endif
void via_custom_value_command(uint8_t *data, uint8_t length); // weak in quantum/via.c
void via_custom_value_command_kb(uint8_t *data, uint8_t length); // svalboard.c
void via_custom_value_command(uint8_t *data, uint8_t length) {
    // data = [ command_id, channel_id, value_id, value_data ]
    if (data[1] == id_qmk_rgblight_channel) {
        if (data[0] == id_custom_set_value || data[0] == id_custom_save) sval_rgb_idle_restore();
        via_qmk_rgblight_command(data, length);
        if (data[0] == id_custom_get_value && data[2] == id_qmk_rgblight_brightness && rgb_idle_applied) {
            data[3] = ((uint16_t)rgb_awake_val * UINT8_MAX) / RGBLIGHT_LIMIT_VAL;
        }
        return;
    }
    via_custom_value_command_kb(data, length);
}
#endif

// ---- RGBLIGHT_SLEEP (config.h): the LEDs go dark while the host sleeps. QMK
// does that from suspend_power_down(), which NO_USB_STARTUP_CHECK (rules.mk)
// leaves uncalled; its wake half, rgblight_wakeup(), still runs from the USB
// wake event (usb_main.c), and runs here as well for a suspend left any other
// way. Master only: the other half follows through the split sync.
#if defined(RGBLIGHT_SLEEP) && defined(RGBLIGHT_ENABLE)
static bool rgb_host_asleep;

void sval_rgb_host_sleep_task(void) {
    bool suspended = USB_DRIVER.state == USB_SUSPENDED;
    if (suspended && !rgb_host_asleep) {
        rgblight_suspend();
    } else if (!suspended && rgb_host_asleep) {
        rgblight_wakeup(); // after the wake event's own call it changes nothing
    }
    rgb_host_asleep = suspended;
}
#else
void sval_rgb_host_sleep_task(void) {}
#endif
