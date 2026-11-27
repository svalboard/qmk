// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The decisions behind the idle scan period (matrix.c) and the deep-idle clock
// switch (power.c), kept free of hardware so tests/sval_power_idle can run them
// on a host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- Idle scan period (matrix.c) ----
// The frame period for a quiet spell of quiet_ms: the active period, light idle
// after light_after_ms, deep idle after deep_after_s (0 turns a stage off; a
// stage only counts when its period is longer than the active one). A key that
// is down keeps the active period, however long it is held: its release must be
// seen as quickly as its press, or the host keeps auto-repeating it.
static inline uint32_t sval_idle_period(uint32_t active_us, uint32_t light_us, uint16_t light_after_ms, uint32_t deep_us, uint16_t deep_after_s, uint32_t quiet_ms, bool key_down, uint8_t *stage) {
    uint32_t p = active_us;
    uint8_t  s = 0;
    if (p && !key_down) {
        if (deep_after_s && deep_us > p && quiet_ms > (uint32_t)deep_after_s * 1000u) {
            p = deep_us;
            s = 2;
        } else if (light_after_ms && light_us > p && quiet_ms > light_after_ms) {
            p = light_us;
            s = 1;
        }
    }
    if (stage) *stage = s;
    return p;
}

// ---- PIO dividers across a clock switch (power.c) ----
// The RP2040 has 8 PIO state machines (pio0 sm0-3, pio1 sm0-3). A divider
// register holds a 16.8 fixed-point divider in bits 31:8.
#define SVAL_PIO_SMS 8

typedef struct {
    uint32_t saved[SVAL_PIO_SMS]; // the full-clock divider, saved on the way down
    uint8_t  saved_mask;          // which state machines saved[] holds
    uint8_t  clamped_mask;        // which ones could not keep their rate at the low clock
} sval_pio_reclock_t;

// clkdiv rescaled from from_hz to to_hz so clk_sys / div stays the same. A
// divider below 1.0 or above 65535.996 cannot be set: it is clamped, and
// *clamped is set.
static inline uint32_t sval_pio_div_scale(uint32_t clkdiv, uint32_t from_hz, uint32_t to_hz, bool *clamped) {
    uint64_t fixed  = clkdiv >> 8;
    uint64_t scaled = (fixed * to_hz + from_hz / 2) / from_hz;
    if (scaled < 0x100) {
        scaled = 0x100;
        if (clamped) *clamped = true;
    } else if (scaled > 0xFFFFFF) {
        scaled = 0xFFFFFF;
        if (clamped) *clamped = true;
    }
    return (uint32_t)(scaled << 8);
}

// Would a switch from from_hz to to_hz clamp any enabled state machine?
static inline bool sval_pio_would_clamp(const uint32_t clkdiv[SVAL_PIO_SMS], uint8_t enabled, uint32_t from_hz, uint32_t to_hz) {
    bool clamped = false;
    for (int i = 0; i < SVAL_PIO_SMS; i++) {
        if (enabled & (1u << i)) (void)sval_pio_div_scale(clkdiv[i], from_hz, to_hz, &clamped);
    }
    return clamped;
}

// Down to the low clock: save each enabled state machine's divider and scale it.
static inline void sval_pio_reclock_down(sval_pio_reclock_t *r, uint32_t clkdiv[SVAL_PIO_SMS], uint8_t enabled, uint32_t full_hz, uint32_t low_hz) {
    r->saved_mask   = 0;
    r->clamped_mask = 0;
    for (int i = 0; i < SVAL_PIO_SMS; i++) {
        if (!(enabled & (1u << i))) continue;
        bool clamped = false;
        r->saved[i]  = clkdiv[i];
        r->saved_mask |= (uint8_t)(1u << i);
        clkdiv[i] = sval_pio_div_scale(clkdiv[i], full_hz, low_hz, &clamped);
        if (clamped) r->clamped_mask |= (uint8_t)(1u << i);
    }
}

// Back to the full clock: the saved state machines get their exact saved
// divider back. One started while the clock was low has no saved divider; its
// driver set it for the low clock, so it is scaled up instead.
static inline void sval_pio_reclock_up(sval_pio_reclock_t *r, uint32_t clkdiv[SVAL_PIO_SMS], uint8_t enabled, uint32_t low_hz, uint32_t full_hz) {
    for (int i = 0; i < SVAL_PIO_SMS; i++) {
        if (!(enabled & (1u << i))) continue;
        if (r->saved_mask & (1u << i)) {
            clkdiv[i] = r->saved[i];
        } else {
            clkdiv[i] = sval_pio_div_scale(clkdiv[i], low_hz, full_hz, NULL);
        }
    }
    r->saved_mask   = 0;
    r->clamped_mask = 0;
}

// ---- LED writes and the low clock (power.c) ----
// A WS2812 frame (one LED per half, plus the reset gap) is out well within this
// after the write starts.
#define SVAL_LED_WRITE_SETTLE_MS 5

// An LED write needs the full clock first when the low clock clamped a state
// machine's divider (the WS2812 one at 12 MHz): its bit timing would be off.
static inline bool sval_led_write_needs_full(bool clock_low, uint8_t clamped_mask) {
    return clock_low && clamped_mask != 0;
}

// Going down to a clock that would clamp a divider waits until the last LED
// write has finished at the full clock.
static inline bool sval_clock_low_allowed(bool would_clamp, bool led_written, uint32_t ms_since_led_write) {
    return !would_clamp || !led_written || ms_since_led_write > SVAL_LED_WRITE_SETTLE_MS;
}
