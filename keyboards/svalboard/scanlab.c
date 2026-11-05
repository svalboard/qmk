/*
Copyright 2026 Morgan Venable @_claussen

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
#include "scanlab.h"
#include "svalboard.h"
#include "debounce_sval.h"
#include "split_util.h"
#include "split_common/transactions.h"
#include "link_light.h"
#include <string.h>
#if VIA_ENABLE
#    include "via.h"
#endif
#ifdef SVAL_UPDATER
#    include "updater/updater.h"
#    include "split_pause.h"
#endif

// Microsecond clock for the probe. The ChibiOS system timer on the RP2040
// port runs at 1 MHz and is readable with interrupts disabled.
_Static_assert(CH_CFG_ST_FREQUENCY == 1000000, "scanlab assumes a 1 MHz system timer");
static inline uint32_t now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

// Provided by matrix.c
extern const pin_t col_pins[MATRIX_COLS];
void unselect_row(uint8_t row);

// Hard cap on sample-loop iterations so the probe can never spin forever,
// whatever the clock does. At ~0.5 us per iteration this is far beyond the
// 1.5 ms window.
#define SCANLAB_PROBE_MAX_ITER 200000u

typedef struct {
    uint8_t      state;
    uint16_t     prewait, postwait;
    uint16_t     frames_target, frames_done;
    uint8_t      ref_valid, ref_count, ref_tries;
    matrix_row_t ref_cand[SCANLAB_ROWS];
    matrix_row_t ref[SCANLAB_ROWS];
    matrix_row_t last[SCANLAB_ROWS];
    uint16_t     mism[SCANLAB_ROWS][SCANLAB_COLS];
} scanlab_sweep_t;

typedef struct {
    uint8_t  valid, row;
    uint8_t  idle_level, lit_level, off_level;   // column bitmasks (bit c = column c high)
    uint16_t on_settle[SCANLAB_COLS];            // us after row-on of the last level change (0 = never changed)
    uint16_t off_recover[SCANLAB_COLS];          // us after row-off of the last level change
    uint8_t  on_trans[SCANLAB_COLS];             // number of level changes seen in each phase
    uint8_t  off_trans[SCANLAB_COLS];
} scanlab_probe_t;

static scanlab_sweep_t sweep;
static scanlab_probe_t probe;

// Host-initiated reboot into the bootloader: two stages so a stray packet can
// never do it. ARM hands out a token good for SCANLAB_REBOOT_WINDOW_MS; GO
// with that token acknowledges, then the reboot happens from housekeeping
// 100 ms later so the acknowledgement reaches the host first.
static uint16_t reboot_token    = 0;
static uint32_t reboot_armed_ms = 0;
#if SVAL_HOST_BOOTLOADER
static bool     reboot_pending  = false;
static uint32_t reboot_due_ms   = 0;
#endif

bool scanlab_reboot_armed(void) {
    return reboot_token != 0 && timer_elapsed32(reboot_armed_ms) <= SCANLAB_REBOOT_WINDOW_MS;
}

void scanlab_housekeeping(void) {
#if SVAL_HOST_BOOTLOADER
    if (reboot_pending && (int32_t)(timer_read32() - reboot_due_ms) >= 0) {
        reboot_pending = false;
        reset_keyboard();  // clears the report, then bootloader_jump(): ROM UF2 mode
    }
#endif
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }

void scanlab_init(void) {
    memset(&sweep, 0, sizeof(sweep));
    memset(&probe, 0, sizeof(probe));
}

bool scanlab_active(void) {
    return sweep.state == SCANLAB_REF || sweep.state == SCANLAB_RUN;
}

bool scanlab_timing(uint16_t *pre, uint16_t *post) {
    switch (sweep.state) {
        case SCANLAB_REF:
            *pre  = SCANLAB_SAFE_PREWAIT_US;
            *post = SCANLAB_SAFE_POSTWAIT_US;
            return true;
        case SCANLAB_RUN:
            *pre  = sweep.prewait;
            *post = sweep.postwait;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------- sweep ---

void scanlab_on_frame(const matrix_row_t *raw) {
    if (sweep.state == SCANLAB_REF) {
        if (sweep.ref_count == 0 || memcmp(raw, sweep.ref_cand, sizeof(sweep.ref_cand)) != 0) {
            memcpy(sweep.ref_cand, raw, sizeof(sweep.ref_cand));
            sweep.ref_count = 1;
        } else {
            sweep.ref_count++;
        }
        if (sweep.ref_count >= SCANLAB_REF_FRAMES) {
            memcpy(sweep.ref, sweep.ref_cand, sizeof(sweep.ref));
            sweep.ref_valid   = 1;
            sweep.frames_done = 0;
            memset(sweep.mism, 0, sizeof(sweep.mism));
            sweep.state = SCANLAB_RUN;
        } else if (++sweep.ref_tries >= SCANLAB_REF_MAX_TRIES) {
            sweep.state = SCANLAB_REF_FAIL;
        }
        return;
    }
    if (sweep.state == SCANLAB_RUN) {
        for (uint8_t r = 0; r < SCANLAB_ROWS; r++) {
            matrix_row_t diff = raw[r] ^ sweep.ref[r];
            for (uint8_t c = 0; c < SCANLAB_COLS; c++) {
                if ((diff & (1u << c)) && sweep.mism[r][c] < 0xFFFF) sweep.mism[r][c]++;
            }
            sweep.last[r] = raw[r];
        }
        if (++sweep.frames_done >= sweep.frames_target) sweep.state = SCANLAB_DONE;
    }
}

// ---------------------------------------------------------------- probe ---

static inline uint8_t read_cols(void) {
    uint8_t v = 0;
    for (uint8_t c = 0; c < SCANLAB_COLS; c++) {
        if (col_pins[c] != NO_PIN && gpio_read_pin(col_pins[c])) v |= (1u << c);
    }
    return v;
}

// Sample all columns until `window_us` has elapsed, recording for each column
// the time of its last level change and the number of changes. Returns the
// final column levels. Must run with interrupts disabled for clean timing.
static uint8_t sample_phase(uint8_t start_level, uint32_t t0, uint16_t window_us, uint16_t *last_change, uint8_t *trans) {
    uint8_t  last = start_level;
    uint32_t now;
    uint32_t iter = 0;
    do {
        if (++iter > SCANLAB_PROBE_MAX_ITER) break;
        now        = now_us() - t0;
        uint8_t v  = read_cols();
        uint8_t d  = v ^ last;
        if (d) {
            for (uint8_t c = 0; c < SCANLAB_COLS; c++) {
                if (d & (1u << c)) {
                    last_change[c] = (uint16_t)now;
                    if (trans[c] < 0xFF) trans[c]++;
                }
            }
            last = v;
        }
    } while (now < window_us);
    return last;
}

void scanlab_probe_row(uint8_t row) {
    if (row >= SCANLAB_ROWS) return;
    for (uint8_t r = 0; r < SCANLAB_ROWS; r++) unselect_row(r);
    wait_us(1000);  // let every line reach its idle level

    memset(&probe, 0, sizeof(probe));
    probe.row = row;

    // Nothing inside this block may take the kernel lock again: the row is
    // driven with raw pin writes, not select_row()/unselect_row(), whose own
    // critical sections would re-enter the lock we are holding.
    uint32_t t0;
    ATOMIC_BLOCK_FORCEON {
        probe.idle_level = read_cols();

        t0 = now_us();
        sval_row_drive_raw(row, true);
        probe.lit_level = sample_phase(probe.idle_level, t0, SCANLAB_PROBE_WINDOW_US, probe.on_settle, probe.on_trans);

        t0 = now_us();
        sval_row_drive_raw(row, false);
        probe.off_level = sample_phase(probe.lit_level, t0, SCANLAB_PROBE_WINDOW_US, probe.off_recover, probe.off_trans);
    }
    unselect_row(row);  // restore the normal (locked) output state
    probe.valid = 1;
}

// -------------------------------------------------------------- requests ---

// req: [op][args...] (SCANLAB_REQ_LEN bytes). rsp: SCANLAB_RSP_LEN bytes, zeroed.
void scanlab_handle(const uint8_t *req, uint8_t *rsp) {
    uint8_t op = req[0];
    memset(rsp, 0, SCANLAB_RSP_LEN);

    switch (op) {
        case SCANLAB_OP_SET_MODE: {
            uint8_t mode = req[1];
#ifdef SVAL_UPDATER
            // No host-chosen scan timing while an update session is active:
            // the updater reads its confirmation chord from the matrix.
            if (mode == 1 && updater_active()) {
                rsp[0] = sweep.state;
                return;
            }
#endif
            if (mode == 1) {
                sweep.prewait       = get16(&req[2]);
                sweep.postwait      = get16(&req[4]);
                sweep.frames_target = get16(&req[6]);
                if (sweep.frames_target == 0) sweep.frames_target = 100;
                sweep.ref_valid = sweep.ref_count = sweep.ref_tries = 0;
                sweep.frames_done = 0;
                memset(sweep.mism, 0, sizeof(sweep.mism));
                sweep.state = SCANLAB_REF;
            } else {
                sweep.state = SCANLAB_IDLE;
            }
            rsp[0] = sweep.state;
            return;
        }
        case SCANLAB_OP_PROBE:
#ifdef SVAL_UPDATER
            if (updater_active()) return;  // nor drive rows by hand (rsp all zero: no probe)
#endif
            sweep.state = SCANLAB_IDLE;  // never probe mid-sweep
            scanlab_probe_row(req[1]);
            rsp[0] = probe.valid;
            rsp[1] = probe.row;
            return;
        case SCANLAB_OP_ABORT:
            sweep.state = SCANLAB_IDLE;
            rsp[0] = sweep.state;
            return;
        case SCANLAB_OP_REBOOT_ARM:
#if SVAL_HOST_BOOTLOADER
            reboot_token    = (uint16_t)((timer_read32() * 2654435761u) >> 16) | 1u;
            reboot_armed_ms = timer_read32();
            put16(&rsp[0], reboot_token);
            rsp[2] = 1;  // supported
#else
            rsp[2] = 0;  // not compiled in
#endif
            return;
        case SCANLAB_OP_REBOOT_GO: {
#if SVAL_HOST_BOOTLOADER
            uint16_t token = get16(&req[1]);
            bool     ok    = scanlab_reboot_armed() && token == reboot_token;
            if (ok) {
                reboot_token   = 0;
                reboot_pending = true;
                reboot_due_ms  = timer_read32() + 100;
            }
            rsp[0] = ok ? 1 : 0;
#else
            rsp[0] = 0;
#endif
            rsp[2] = SVAL_HOST_BOOTLOADER ? 1 : 0;
            return;
        }
        case SCANLAB_OP_STATUS: {
            uint16_t pre, post;
            sval_scan_timing(&pre, &post);
            rsp[0] = SCANLAB_PROTO_VERSION;
            rsp[1] = sval_hw_rev();
            rsp[2] = sweep.state;
            put16(&rsp[3], sweep.frames_done);
            put16(&rsp[5], sweep.frames_target);
            rsp[7] = sweep.ref_valid;
            put16(&rsp[8], pre);
            put16(&rsp[10], post);
            rsp[12] = is_keyboard_left() ? 1 : 0;
            rsp[13] = sval_pushed_mask(false);
            rsp[14] = sval_pushed_mask(true);
            rsp[15] = probe.valid;
            rsp[16] = probe.row;
            put16(&rsp[17], global_saved_values.scan_prewait_us);
            put16(&rsp[19], global_saved_values.scan_postwait_us);
            rsp[21] = sval_timing_preset(global_saved_values.scan_prewait_us, global_saved_values.scan_postwait_us); // the preset, or 0xFF
            rsp[22] = is_keyboard_master() ? (sval_other_half_connected() ? 1 : 0) : 0;
            // Capabilities: bit 0, the optical debounce switch (VIA value 48);
            // bit 1, this half debounces its optical keys now. The half
            // without USB reports the mode it took from the presence ping.
            rsp[23] = SCANLAB_CAP_DEBOUNCE_SWITCH | (sval_optical_debounce_on() ? SCANLAB_CAP_OPTICAL_DEBOUNCE_ON : 0);
            return;
        }
        case SCANLAB_OP_IDLE: {
            sval_idle_status_t st;
            sval_idle_status(&st);
            uint8_t stage;
            sval_scan_period_now(&stage);
            rsp[0] = global_saved_values.idle_flags;
            rsp[1] = st.sensor_present;
            rsp[2] = st.sensor_mode;
            rsp[3] = st.sensor_config2;
            rsp[4] = st.rgb_val_now;
            rsp[5] = st.rgb_val_awake;
            rsp[6] = st.rgb_stage;
            rsp[7] = st.rgb_enabled;
            rsp[8] = stage;
            put32(&rsp[9],  last_input_activity_elapsed());
            put32(&rsp[13], last_matrix_activity_elapsed());
            put32(&rsp[17], last_pointing_device_activity_elapsed());
            rsp[21] = sval_clock_mhz();
            rsp[22] = sval_deep_clock_mhz();
            return;
        }
        case SCANLAB_OP_POWER: {
            uint32_t frame_us;
            uint16_t led_us, pre, post;
            uint8_t  stage, stage_now;
            sval_scan_stats(&frame_us, &led_us, &stage);
            sval_scan_timing(&pre, &post);
            uint32_t eff = sval_scan_period_now(&stage_now);
            put16(&rsp[0], global_saved_values.scan_period_us);
            put16(&rsp[2], global_saved_values.scan_idle_period_ms);
            put16(&rsp[4], global_saved_values.scan_idle_after_ms);
            put16(&rsp[6], frame_us > 0xFFFF ? 0xFFFF : (uint16_t)frame_us);
            put16(&rsp[8], led_us);
            rsp[10] = stage;  // 0 active, 1 light idle, 2 deep idle
            put16(&rsp[11], eff > 0xFFFF ? 0xFFFF : (uint16_t)eff);
            put16(&rsp[13], pre);
            put16(&rsp[15], post);
            rsp[17] = SCANLAB_ROWS;
            put16(&rsp[18], global_saved_values.scan_deep_after_s);
            put16(&rsp[20], global_saved_values.scan_deep_period_ms);
            rsp[22] = (SVAL_HOST_BOOTLOADER ? 1 : 0) | (scanlab_reboot_armed() ? 2 : 0) |
                      ((global_saved_values.idle_flags & 0x1F) << 2); // bit2 pointer rest, bit3 RGB dim, bit4 CPU sleep, bit5 low clock, bit6 long naps
            return;
        }
        default:
            break;
    }

    uint8_t kind = op & 0xE0;
    uint8_t row  = op & 0x07;
    if (row >= SCANLAB_ROWS) return;

    if (kind == SCANLAB_OP_SWEEP_ROW) {
        for (uint8_t c = 0; c < SCANLAB_COLS; c++) put16(&rsp[2 * c], sweep.mism[row][c]);
        rsp[12] = sweep.ref[row];
        rsp[13] = sweep.last[row];
        rsp[14] = sweep.state;
        rsp[15] = sweep.ref_valid;
        return;
    }
    if (kind == SCANLAB_OP_PROBE_ON) {
        if (!probe.valid || probe.row != row) return;  // all zero: no data for this row
        for (uint8_t c = 0; c < SCANLAB_COLS; c++) put16(&rsp[2 * c], probe.on_settle[c]);
        memcpy(&rsp[12], probe.on_trans, SCANLAB_COLS);
        rsp[18] = probe.idle_level;
        rsp[19] = probe.lit_level;
        rsp[20] = 1;  // valid
        return;
    }
    if (kind == SCANLAB_OP_PROBE_OFF) {
        if (!probe.valid || probe.row != row) return;
        for (uint8_t c = 0; c < SCANLAB_COLS; c++) put16(&rsp[2 * c], probe.off_recover[c]);
        memcpy(&rsp[12], probe.off_trans, SCANLAB_COLS);
        rsp[18] = probe.off_level;
        rsp[19] = probe.lit_level;
        rsp[20] = 1;
        return;
    }
}

// ------------------------------------------------------------ VIA (master) ---

#if VIA_ENABLE
// data = [command_id][channel][value_id][value_data...]; response is written over value_data.
void scanlab_via_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd  = data[0];
    uint8_t  id   = data[2];
    uint8_t *args = &data[3];
    uint8_t  req[SCANLAB_REQ_LEN] = {0};
    uint8_t  rsp[SCANLAB_RSP_LEN] = {0};
    uint8_t  hand;

    if (cmd == id_custom_set_value) {
        hand   = args[0] & 1;
        req[0] = id;
        memcpy(&req[1], &args[1], SCANLAB_REQ_LEN - 1);
    } else if (cmd == id_custom_get_value) {
        hand   = (id >> 3) & 1;
        req[0] = id & ~0x08;
    } else {
        return;  // id_custom_save: nothing to persist here
    }

    bool local = (hand == 0) == is_keyboard_left();
    if (local) {
        scanlab_handle(req, rsp);
#ifdef SVAL_UPDATER
    } else if (sval_split_paused()) {
        // The split link is paused for an update (D15): no relay, answered as
        // if the other half were silent.
        memset(rsp, 0, sizeof(rsp));
        rsp[0] = 0xFF;
#endif
    } else {
        // The other half goes to its bootloader on purpose: no link light for it.
        if (req[0] == SCANLAB_OP_REBOOT_GO) sval_link_stopped();
        if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(rsp), rsp)) {
            memset(rsp, 0, sizeof(rsp));
            rsp[0] = 0xFF;  // other half did not answer
        }
    }

    uint8_t room = length > 3 ? length - 3 : 0;
    memcpy(args, rsp, room < SCANLAB_RSP_LEN ? room : SCANLAB_RSP_LEN);
}
#else
void scanlab_via_command(uint8_t *data, uint8_t length) {
    (void)data;
    (void)length;
}
#endif
