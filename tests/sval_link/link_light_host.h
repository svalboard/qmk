// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Mocks for link_light.c on the host (test_link_light.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

extern uint32_t host_now_ms;
extern bool     host_master, host_transport_up, host_other_connected, host_usb_configured, host_updater_active;
// Called at every timer read, once the value is taken: a test runs the presence
// ping there, as the SlaveThread preempting the main loop would.
extern void (*host_on_timer)(void);

static inline uint32_t timer_read32(void) {
    uint32_t now = host_now_ms;
    if (host_on_timer) host_on_timer();
    return now;
}
static inline uint32_t timer_elapsed32(uint32_t t) {
    uint32_t e = host_now_ms - t;
    if (host_on_timer) host_on_timer();
    return e;
}
static inline bool is_keyboard_master(void) {
    return host_master;
}
static inline bool is_transport_connected(void) {
    return host_transport_up;
}
static inline bool sval_other_half_connected(void) {
    return host_other_connected;
}
#define USB_DEVICE_STATE_CONFIGURED 2
static inline int usb_device_state_get_configure_state(void) {
    return host_usb_configured ? USB_DEVICE_STATE_CONFIGURED : 3;
}
#define SVAL_UPDATER
static inline bool updater_active(void) {
    return host_updater_active;
}
