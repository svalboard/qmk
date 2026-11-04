// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The split link's disconnection check (split_util.c). With min_ms 0 the link
// counts as disconnected after max_errors consecutive failed scans (stock
// QMK). With min_ms above 0 the time replaces the count: it is disconnected
// once consecutive failures have gone on for min_ms, however many there were
// (counted from the end of the first failed scan). That keeps the disconnect
// independent of how long one failed scan takes, from a few ms (a fast
// failure at a short deadline) to tens of ms (a timeout with retries); a
// disconnect releases the other half's held keys. One good scan connects again.
typedef struct {
    uint8_t  errors;       // consecutive failed scans
    bool     failing;      // a run of failures has started
    bool     disconnected; // the check fired
    uint16_t since;        // when the run started (timer_read, ms)
} split_connection_t;

static inline bool split_connection_update(split_connection_t *c, bool ok, uint16_t now, uint8_t max_errors, uint16_t min_ms) {
    if (ok) {
        c->errors       = 0;
        c->failing      = false;
        c->disconnected = false;
        return true;
    }
    if (!c->failing) {
        c->failing = true;
        c->since   = now;
    }
    if (c->errors < UINT8_MAX) c->errors++;
    // Once fired it holds until a good scan, so the 16-bit timer can wrap.
    if (min_ms ? (uint16_t)(now - c->since) >= min_ms : c->errors >= max_errors) c->disconnected = true;
    return !c->disconnected;
}
