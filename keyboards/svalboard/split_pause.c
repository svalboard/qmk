// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Split pause (D15): see split_pause.h. Built only with SVAL_UPDATER=yes.
//
// This overrides QMK's weak matrix_scan() (quantum/matrix_common.c). Lines
// marked "core:" copy the core original and must be checked against it on
// every QMK merge. It lives in its own file, not in kb/matrix.c, because
// kb/matrix.c declares the core raw_matrix and matrix arrays with five rows
// (ROWS_PER_HAND) while core defines them with MATRIX_ROWS (ten); fixing those
// declarations there would change the default builds (its first-scan memset
// is sized by them), so this file declares them as core does.

#ifdef SVAL_UPDATER_HOST_TEST
#    include "split_pause_host.h" // tests/sval_updater: matrix, transport and pointing mocks
#else
#    include <string.h>
#    include "quantum.h"
#    include "matrix.h"
#    include "debounce.h"
#    include "split_common/split_util.h"
#    if defined(POINTING_DEVICE_ENABLE) && defined(SPLIT_POINTING_ENABLE)
#        include "pointing_device.h"
#    endif
#    ifndef SPLIT_KEYBOARD
#        error "split_pause.c is for the split Svalboard"
#    endif
#endif
#include "split_pause.h"
#ifdef SVAL_PERF_STATS
#    include "perf.h" // the split exchange's share of a scan gap
#endif

// core: quantum/matrix_common.c defines these (not static).
extern matrix_row_t raw_matrix[MATRIX_ROWS];
extern matrix_row_t matrix[MATRIX_ROWS];
extern uint8_t      thisHand, thatHand;
#ifndef SVAL_UPDATER_HOST_TEST
// core: quantum/matrix_common.c (weak; kb/matrix.c defines it). No core header declares it.
bool matrix_scan_custom(matrix_row_t current_matrix[]);
// updater/update_split.h: this half (without USB) receives a relay on the fast path.
bool update_split_slave_frozen(void);
#endif

static bool paused;      // default false: bootmagic calls matrix_scan() before anything else runs
static bool entry_clear; // the first paused scan still has to release the other half's keys

bool sval_split_pause(bool on) {
    if (!on) {
        if (paused) {
            paused      = false;
            entry_clear = false;
            // A half that rebooted while the link was paused (an update's
            // commit) waits for a watchdog ping, and resets itself again
            // without one (quantum/split_common/split_util.c): re-arm it, so
            // the next exchange pings.
            split_watchdog_update(false);
        }
        return true;
    }
    if (paused) return true;
    if (!is_keyboard_master() || !is_transport_connected()) return false;
    paused      = true;
    entry_clear = true;
    return true;
}

bool sval_split_paused(void) {
    return paused;
}

uint8_t matrix_scan(void) {
    bool changed = false;
    if (update_split_slave_frozen()) {
        // The half without USB while it takes a relay on the fast path: no
        // scan, its keys read released (the rest below as usual), so the
        // pages and the hash get the time, and no key held now is reported
        // as held when the link resumes.
        for (uint8_t i = 0; i < MATRIX_ROWS_PER_HAND; i++) {
            changed |= raw_matrix[i] != 0;
            raw_matrix[i] = 0;
        }
    } else {
        // core: matrix_scan(), CUSTOM_MATRIX lite
        changed = matrix_scan_custom(raw_matrix);
    }

    if (!paused) {
        // core: matrix_scan(), SPLIT_KEYBOARD
        changed = debounce(raw_matrix, matrix + thisHand, changed);
#ifdef SVAL_PERF_STATS
        uint8_t perf_prev = sval_perf_enter(SVAL_PERF_GAP_SYNC);
#endif
        changed |= matrix_post_scan();
#ifdef SVAL_PERF_STATS
        sval_perf_leave(perf_prev);
#endif
        return changed;
    }

    // Paused: this half as usual, no split exchange. matrix_post_scan() on
    // the master is the exchange plus matrix_scan_kb(), so only the latter runs.
    changed = debounce(raw_matrix, matrix + thisHand, changed);
    if (entry_clear) {
        // core: matrix_post_scan(), the disconnect path: the other half's
        // rows go to zero once and a change is reported, so any of its keys
        // held now are released rather than stuck for the whole pause.
        memset(matrix + thatHand, 0, MATRIX_ROWS_PER_HAND * sizeof(matrix_row_t));
        changed = true;
#if defined(POINTING_DEVICE_ENABLE) && defined(SPLIT_POINTING_ENABLE)
        // Not in core's disconnect path: the master keeps applying the other
        // half's last pointing report until a new one arrives, so motion in
        // flight at pause entry would repeat for the whole pause.
        pointing_device_set_shared_report((report_mouse_t){0});
#endif
        entry_clear = false;
    }
    matrix_scan_kb();
    return changed;
}
