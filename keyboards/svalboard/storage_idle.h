// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// When to consolidate the settings store early (svalboard.c), kept free of
// hardware so tests/sval_storage_idle can run it on a host.
//
// A consolidation pauses the board for a few seconds (docs/settings-storage.md).
// When the write log fills inside a burst of host writes (a layout import),
// that pause holds back the reply to the write that filled it, past the host's
// timeout. Consolidating while the board is idle, once the log is past half
// full, empties the log before the next burst, so a burst rarely fills it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool done; // consolidated in this quiet spell: not again until activity
} sval_storage_idle_t;

// Whether to consolidate now: the log has less than log_low bytes free, no raw
// HID packet of any kind (a polling host never counts as idle) and no input for
// idle_ms, and nothing else is using the flash (busy). At most once per quiet
// spell, so a consolidation that fails isn't retried until the board is used.
static inline bool sval_storage_idle_due(sval_storage_idle_t *st, uint32_t log_free, uint32_t log_low, uint32_t host_idle_ms, uint32_t input_idle_ms, uint32_t idle_ms, bool busy) {
    if (host_idle_ms < idle_ms || input_idle_ms < idle_ms) {
        st->done = false;
        return false;
    }
    if (busy || st->done || log_free >= log_low) return false;
    st->done = true;
    return true;
}
