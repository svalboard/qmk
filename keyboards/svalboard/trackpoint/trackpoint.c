// TrackPoint (PS/2) - Sprintek-specific configuration
// Uses svalboard/pointing_device_ps2 module for PS/2 integration

#include "quantum.h"
#include "pointing_device.h"
#include "pointing_device_ps2.h"
#include "ps2.h"
#include "svalboard.h"
#include "pointing_detect.h"
#include "pointer_speed.h"

// Whether a PS/2 device answered Get Device ID at init, and the ID it sent.
static bool    tp_found = false;
static uint8_t tp_id    = 0;

void sval_pointer_identity(sval_pointer_identity_t *id) {
    id->built_for  = SVAL_SENSOR_TRACKPOINT;
    id->detected   = tp_found ? SVAL_SENSOR_TRACKPOINT : SVAL_SENSOR_NO_RESPONSE;
    id->product_id = tp_id;
    if (tp_found) id->flags |= SVAL_POINTER_INIT_OK;
}

uint8_t sval_pointer_own_bus(void) {
    return SVAL_BUS_PS2;
}

#ifndef SVAL_POINTING_AUTO
// The PS/2 driver claims a PIO state machine and program each time it starts, so a
// TrackPoint build doesn't start it again: after the boot search, it stops looking.
uint8_t sval_pointer_retry_own(void) {
    return tp_found ? SVAL_RETRY_OK : SVAL_RETRY_STOP;
}
#endif

// The Pointer speed, as a DPI (800 is 1:1; pointer_speed.h). 0 until the half
// with the board's settings sends one, which is also 1:1. A TrackPoint has no
// DPI to set, so the speed is applied here, to its counts.
static uint16_t        tp_cpi = 0;
static sval_tp_scale_t tp_scale;

static void sval_tp_apply_cpi(uint16_t cpi) {
    tp_cpi = cpi;
}

#ifndef SVAL_POINTING_AUTO
void pointing_device_driver_set_cpi(uint16_t cpi) {
    sval_tp_apply_cpi(cpi);
}

uint16_t pointing_device_driver_get_cpi(void) {
    return tp_cpi;
}
#endif

// Override get_report to apply axis rotation for svalboard orientation, then the Pointer speed
report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    mouse_report = ps2_mouse_get_report(mouse_report);

    // 90° rotation: x=y, y=-x
    int16_t swap = mouse_report.x;
    mouse_report.x = mouse_report.y;
    mouse_report.y = -swap;

    // At 800 (and before any speed is set) the counts pass through untouched.
    int32_t x, y;
    sval_tp_scale(&tp_scale, tp_cpi, mouse_report.x, mouse_report.y, &x, &y);
    const int32_t lim = (1L << (8 * sizeof(mouse_xy_report_t) - 1)) - 1;
    mouse_report.x    = x > lim ? lim : x < -lim ? -lim : x;
    mouse_report.y    = y > lim ? lim : y < -lim ? -lim : y;

    return mouse_report;
}

void pointing_device_init_kb(void) {
    // TrackPoint-specific initialization
    // Disable Z-tap
    ps2_mouse_disable_data_reporting();

    // Get Device ID: a PS/2 device ACKs (0xFA) and sends its ID.
    tp_found = ps2_host_send(0xF2) == 0xFA;
    tp_id    = tp_found ? ps2_host_recv_response() : 0;

    PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
    PS2_MOUSE_SEND(0x47, "pts: 0x47");
    PS2_MOUSE_SEND(0x2C, "pts: 0x2C");
    PS2_MOUSE_SEND(0x00, "pts: 0x00");  // 01 turns on Z tap, 00 turns off
    PS2_MOUSE_RECEIVE("ztap - disable");

    // Disable drift correction (Sprintek command)
    uint8_t val;
    PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
    PS2_MOUSE_SEND(0x80, "pts: 0x80");
    PS2_MOUSE_SEND(0xFA, "pts: 0xFA");
    val = ps2_host_recv_response();

    if (~val & 0x10) {
        PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
        PS2_MOUSE_SEND(0x47, "pts: 0x47");
        PS2_MOUSE_SEND(0xFA, "pts: 0xFA");
        PS2_MOUSE_SEND(0x10, "pts: 0x10");
        PS2_MOUSE_RECEIVE("drift correction - disable");
    }

    // Set deadzone (increase to reduce tail drift)
    PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
    PS2_MOUSE_SEND(0x81, "pts: 0x81");
    PS2_MOUSE_SEND(0x92, "pts: 0x92");
    PS2_MOUSE_SEND(0x15, "pts: 0x15");  // Experiment with this value if needed

    // Disable double Z tap
    PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
    PS2_MOUSE_SEND(0x81, "pts: 0x81");
    PS2_MOUSE_SEND(0x5E, "pts: 0x5E");
    PS2_MOUSE_SEND(0x00, "pts: 0x00");
    PS2_MOUSE_RECEIVE("double Z tap - disable");

    ps2_mouse_enable_data_reporting();

    pointing_device_init_user();
}

void recalibrate_pointer(void) {
    PS2_MOUSE_SEND(0xE2, "pts: 0xE2");
    PS2_MOUSE_SEND(0x51, "pts: 0x51");
    PS2_MOUSE_RECEIVE("trackpoint - recalibrate");
}
