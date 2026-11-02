/*
Copyright 2012-2018 Jun Wako, Jack Humbert, Yiancar

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
#include "flicker_latch.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "util.h"
#include "matrix.h"
#include "debounce.h"
#include "quantum.h"
#include "print.h"
#include "svalboard.h"
#include "scanlab.h"
#ifdef SVAL_UPDATER
#    include "updater/updater.h"
#    include "updater/update_gesture.h"
#    define sval_updater_busy() updater_active()
#else
#    define sval_updater_busy() false
#endif

#define ROWS_PER_HAND 5

// matrix code
const pin_t col_pins[MATRIX_COLS] = MATRIX_COL_PINS;
const pin_t row_pins[ROWS_PER_HAND] = MATRIX_ROW_PINS;
//static const uint8_t col_pushed_states[MATRIX_COLS] = MATRIX_COL_PUSHED_STATES;
static const uint8_t col_pushed_states_fingers[MATRIX_COLS] = MATRIX_COL_PUSHED_STATES;
static const uint8_t col_pushed_states_thumbs[MATRIX_COLS] = MATRIX_COL_PUSHED_STATES_THUMBS;
static const uint8_t col_pushed_states_thumbs_flipfet[MATRIX_COLS] = MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET;

// Pushed-state table for the thumb row, chosen by hardware revision.
static inline const uint8_t *thumb_pushed_states(void) {
    return sval_hw_rev_is_flipfet() ? col_pushed_states_thumbs_flipfet : col_pushed_states_thumbs;
}

uint8_t sval_pushed_mask(bool thumbs) {
    const uint8_t *t = thumbs ? thumb_pushed_states() : col_pushed_states_fingers;
    uint8_t m = 0;
    for (uint8_t c = 0; c < MATRIX_COLS; c++) if (t[c]) m |= (1u << c);
    return m;
}

static inline void setPinOutput_writeLow(pin_t pin) {
    ATOMIC_BLOCK_FORCEON {
        gpio_set_pin_output(pin);
        gpio_write_pin_low(pin);
    }
}

static inline void setPinOutput_writeHigh(pin_t pin) {
    ATOMIC_BLOCK_FORCEON {
        gpio_set_pin_output(pin);
        gpio_write_pin_high(pin);
    }
}

static inline void setPinInputHigh_atomic(pin_t pin) {
    ATOMIC_BLOCK_FORCEON {
        gpio_set_pin_input_high(pin);
    }
}

static inline uint8_t readMatrixPin(pin_t pin) {
    if (pin != NO_PIN) {
        return (gpio_read_pin(pin));
    } else {
        return 1;
    }
}


bool select_row(uint8_t row) {
    pin_t pin = row_pins[row];
    if (pin != NO_PIN) {
#ifdef PFET_ROWS
        setPinOutput_writeLow(pin);  //
        return true;
#else
        setPinOutput_writeHigh(pin);  //  this is opposite of most KB matrices
        return true;
#endif
    }
    return false;
}

void unselect_row(uint8_t row) {
    pin_t pin = row_pins[row];
#ifdef PFET_ROWS
            setPinOutput_writeHigh(pin);
#else
            setPinOutput_writeLow(pin);
#endif
}


// Drive a row on/off with plain pin writes and no critical section. The row pin
// must already be configured as an output (unselect_row() does that). For use
// inside code that has already disabled interrupts, e.g. the Scan Lab probe.
void sval_row_drive_raw(uint8_t row, bool on) {
    pin_t pin = row_pins[row];
    if (pin == NO_PIN) return;
#ifdef PFET_ROWS
    if (on) gpio_write_pin_low(pin); else gpio_write_pin_high(pin);
#else
    if (on) gpio_write_pin_high(pin); else gpio_write_pin_low(pin);
#endif
}

static void unselect_rows(void) {
    for (uint8_t x = 0; x < ROWS_PER_HAND; x++) {
        unselect_row(x);
    }
}


/* matrix state(1:on, 0:off) */
extern matrix_row_t raw_matrix[ROWS_PER_HAND]; // raw values
extern matrix_row_t matrix[ROWS_PER_HAND];     // debounced values

extern uint8_t sval_ping_turbo; // svalboard.c

// Scan timing for this frame: the Scan Lab override while a sweep runs, else
// the saved waits. A wait is 0 only when an older half with USB sent it; that
// half's turbo index fills it in, as it did (sval_timing_effective).
void sval_scan_timing(uint16_t *pre, uint16_t *post) {
    if (scanlab_timing(pre, post)) return;
    sval_timing_effective(global_saved_values.scan_prewait_us, global_saved_values.scan_postwait_us, sval_ping_turbo, pre, post);
}
static uint16_t cur_prewait_us = 90, cur_postwait_us = 90;

// --- Frame pacing ----------------------------------------------------------
// Sensor LED duty cycle = rows x (pre-wait + read) / frame period, so the frame
// period is the power knob. The gate is non-blocking: when it is too early for
// the next frame we report "no change" and let the main loop service USB, the
// pointing device and the split link. Two idle stages stretch the period
// after a quiet spell: light idle (scan_idle_after_ms -> scan_idle_period_ms,
// e.g. a 100 ms wake-up) and deep idle (scan_deep_after_s ->
// scan_deep_period_ms, seconds-long timeouts, up to a 65 s period). The first
// raw matrix change restores the active period on the next frame, so the only
// latency cost is one idle-length frame on the first key after a pause.
_Static_assert(CH_CFG_ST_FREQUENCY == 1000000, "matrix pacing assumes a 1 MHz system timer");
static inline uint32_t now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

static uint32_t frame_start_us = 0;
static uint32_t last_change_ms = 0;
static uint32_t led_on_acc_us  = 0;
static uint32_t stat_frame_us  = 0;   // measured frame-to-frame interval, smoothed
static uint16_t stat_led_us    = 0;   // measured LED-on time per frame, smoothed
static uint8_t  idle_stage     = 0;   // 0 active, 1 light idle, 2 deep idle

static inline void ema16(uint16_t *s, uint32_t sample) {
    if (sample > 0xFFFF) sample = 0xFFFF;
    *s = (uint16_t)(*s + ((int32_t)sample - (int32_t)*s) / 8);
}
static inline void ema32(uint32_t *s, uint32_t sample) {
    *s = (uint32_t)((int64_t)*s + ((int64_t)sample - (int64_t)*s) / 8);
}

uint32_t sval_scan_period_now(uint8_t *stage) {
    uint32_t p = global_saved_values.scan_period_us;
    uint8_t  s = 0;
    if (p) {
        // Quiet since the last raw matrix change here or any input anywhere (keys on the
        // other half, ball motion), whichever is more recent.
        uint32_t quiet_ms = timer_elapsed32(last_change_ms);
        uint32_t input_ms = last_input_activity_elapsed();
        if (input_ms < quiet_ms) quiet_ms = input_ms;
        uint32_t deep_us  = (uint32_t)global_saved_values.scan_deep_period_ms * 1000u;
        uint32_t light_us = (uint32_t)global_saved_values.scan_idle_period_ms * 1000u;
        if (global_saved_values.scan_deep_after_s && deep_us > p &&
            quiet_ms > (uint32_t)global_saved_values.scan_deep_after_s * 1000u) {
            p = deep_us;
            s = 2;
        } else if (global_saved_values.scan_idle_after_ms && light_us > p &&
                   quiet_ms > global_saved_values.scan_idle_after_ms) {
            p = light_us;
            s = 1;
        }
    }
    if (stage) *stage = s;
    return p;
}

void sval_scan_stats(uint32_t *frame_us, uint16_t *led_us, uint8_t *stage) {
    *frame_us = stat_frame_us;
    *led_us   = stat_led_us;
    *stage    = idle_stage;
}

int16_t scans_before_dd_detect = 3;  // Has to be one higher than the actual number of scans.
uint8_t dd_detected = 0;
void matrix_read_cols_on_row(matrix_row_t current_matrix[], uint8_t current_row) {
    // Start with a clear matrix row
    matrix_row_t current_row_value = 0;

    uint32_t t_on = now_us();
    select_row(current_row);
    // if thumb row use col_pushed_states_thumbs

    wait_us(cur_prewait_us);
    const uint8_t *thumbs = thumb_pushed_states();

    // For each col...
    for (uint8_t col_index = 0; col_index < MATRIX_COLS; col_index++) {
        uint8_t pin_state;
        if (current_row == 0) {
            pin_state = (gpio_read_pin(col_pins[col_index]) == thumbs[col_index]) ? 1 : 0;  // read pin and match pushed_states define
	    if (col_index == 5) { // DD
		if (scans_before_dd_detect >= 0) {
                   scans_before_dd_detect--;
		}
		if (!scans_before_dd_detect && scans_before_dd_detect == 0) {
		    dd_detected = pin_state;
		    scans_before_dd_detect--;
		}
		pin_state ^= dd_detected;
		pin_state &= 1;
	    }
        } else {
               pin_state = (gpio_read_pin(col_pins[col_index]) == col_pushed_states_fingers[col_index]) ? 1 : 0; // read pin and match pushed_states define
        }
	// Populate the matrix row with the state of the col pin
        current_row_value |= (pin_state << col_index);
    }

    // Unselect row
    unselect_row(current_row);
    led_on_acc_us += now_us() - t_on;
    wait_us(cur_postwait_us);

    // Update the matrix
    current_matrix[current_row] = current_row_value;
}

void matrix_init_custom(void) {
    dprint("matrix_init_custom\n");
    unselect_rows();
    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        pin_t pin = col_pins[col];
        if (pin != NO_PIN) {
            if (col == DOUBLEDOWN_COL){
                gpio_set_pin_input_high(pin);
            } else {
                gpio_set_pin_input(pin);
            }
        }
    }
    // SvalMCUMini V1.0 only: which clusters it can scan (mainboard.c), ~1-4 ms.
    sval_cluster_check();
}
bool first_scan = true;
bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    
    matrix_row_t curr_matrix[ROWS_PER_HAND] = {0};

    // Pacing gate (sweeps run unpaced so they finish quickly). An update in
    // progress gets the same treatment: full clock, no deep idle or long naps.
    uint32_t now = now_us();
    if (!scanlab_active() && !sval_updater_busy()) {
        uint32_t period  = sval_scan_period_now(&idle_stage);
        uint8_t  flags   = global_saved_values.idle_flags;
        // A config app mid-conversation needs the main loop running at its normal rate: the
        // deep-idle nap length is also the raw HID round-trip time, and a bootstrap is hundreds
        // of round trips. Deep idle is not left, so the scan period and the sensor LEDs stay
        // where they are; only the clock and the nap length come back.
        bool     host_busy = sval_host_recent();
        bool     deep_pwr  = (idle_stage == 2) && !host_busy;
        // Deep idle may run the core slowly, once the RGB has finished its last write at full
        // speed; any other state needs full speed first.
        if (deep_pwr && (flags & SVAL_IDLE_LOW_CLOCK)) {
            if (sval_rgb_idle_quiesced()) sval_clock_low();
        } else {
            sval_clock_full();
        }
        uint32_t elapsed = now - frame_start_us;
        if (period && elapsed < period) {
            // Not due yet. With CPU sleep on, nap until the frame is due instead of spinning;
            // the idle thread parks the core in WFI. Naps are capped (1 ms normally, longer in
            // deep idle) so USB, the pointer and the split link stay responsive. Wake a little
            // early and let the next pass through here start the frame on time.
            uint32_t remaining = period - elapsed;
            if (!(flags & SVAL_IDLE_CPU_SLEEP) || remaining <= SVAL_SLEEP_MIN_US) return false;
            uint32_t cap = (deep_pwr && (flags & SVAL_IDLE_LONG_NAP)) ? sval_deep_nap_us() : SVAL_SLEEP_MAX_US;
            if (remaining > cap) {
                chThdSleepMicroseconds(cap);
                return false;
            }
            // Last nap before the frame: wake slightly early, finish with a short busy-wait and
            // start the frame right here. Returning and coming back costs a whole loop pass
            // (pointer read, USB, housekeeping) and started frames ~300 us late.
            chThdSleepMicroseconds(remaining - (SVAL_SLEEP_MIN_US / 2));
            while ((uint32_t)(now_us() - frame_start_us) < period) {}
            now = now_us();
        }
    } else {
        idle_stage = 0;
        sval_clock_full();
    }
    if (frame_start_us) ema32(&stat_frame_us, now - frame_start_us);
#ifdef SVAL_PERF_STATS
    extern void sval_perf_scan(uint32_t now_us);
    sval_perf_scan(now);
#endif
    frame_start_us = now;
    led_on_acc_us  = 0;

    sval_scan_timing(&cur_prewait_us, &cur_postwait_us);
#ifdef SVAL_UPDATER
    // While the chord is awaited, never shorter than Scan Lab's safe timing:
    // the timing above is the host's to choose.
    _Static_assert(SVAL_UPDATE_CHORD_PREWAIT_US >= SCANLAB_SAFE_PREWAIT_US && SVAL_UPDATE_CHORD_POSTWAIT_US >= SCANLAB_SAFE_POSTWAIT_US, "chord timing");
    update_gesture_timing(&cur_prewait_us, &cur_postwait_us);
#endif
    // Set row, read cols
    for (uint8_t current_row = 0; current_row < (ROWS_PER_HAND); current_row++) {
        matrix_read_cols_on_row(curr_matrix, current_row);
    }
    ema16(&stat_led_us, led_on_acc_us);
    // SvalMCUMini V1.0: an original cluster can't be scanned, so its keys are muted
    // (its center would read as held). Scan Lab still sees the raw rows.
    uint8_t muted = sval_muted_rows();
    if (muted && !scanlab_active()) {
        for (uint8_t r = 0; r < ROWS_PER_HAND; r++) {
            if (muted & (1u << r)) curr_matrix[r] = 0;
        }
    }
#ifdef SVAL_PERF_STATS
    // Raw frames with a key down, and raw changes (keyboards/svalboard/perf.c).
    extern void sval_perf_matrix(const matrix_row_t *rows, uint8_t count);
    sval_perf_matrix(curr_matrix, ROWS_PER_HAND);
#endif
#ifdef SVAL_UPDATER
    // The updater's confirmation chord is read here, and its keys removed,
    // before debounce and key processing see this frame.
    update_gesture_scan(curr_matrix);
#endif

    // While a Scan Lab sweep runs, frames feed the engine and never become key events.
    if (scanlab_active()) {
        scanlab_on_frame(curr_matrix);
        return false;
    }

    // The first scan comes out backwards for reasons we don't understand.
    // Skipping it, stops the lockup experienced with thumbs out on
    // board boot.
    if (first_scan) {
        memset(raw_matrix, 0, sizeof(raw_matrix));
        first_scan = false;
	return true;
    } else {
        sval_flicker_frame(curr_matrix, ROWS_PER_HAND); // raw, before debounce (Sval 0x3D)
        bool changed = memcmp(raw_matrix, curr_matrix, sizeof(curr_matrix)) != 0;
        if (changed) {
            memcpy(raw_matrix, curr_matrix, sizeof(curr_matrix));
            last_change_ms = timer_read32();
        }
	return changed;
    }
}
