// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad (SVAL_NATIVE_PTP): the USB half presents both
// Azoteq pads as one touchpad on its own HID interface. See
// keyboards/svalboard/docs/native-touchpad.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Who owns the pads. NATIVE is the build default; the dev command
// (SVAL_TOUCH_DEV, raw HID 0xFC op 0x42) can switch to MOUSE.
enum sval_touch_owner {
    SVAL_TOUCH_OWNER_MOUSE    = 0, // the chip's relative mouse
    SVAL_TOUCH_OWNER_HOST     = 1, // retired: host injection (refused)
    SVAL_TOUCH_OWNER_DETECTOR = 2, // reserved (refused)
    SVAL_TOUCH_OWNER_NATIVE   = 3, // the native Precision Touchpad (this file)
};

// Tunables. Build-time defaults; azoteq/ptp_config.h sets them for real builds.
#ifndef SVAL_PTP_PAD_PERIOD_MS
#    define SVAL_PTP_PAD_PERIOD_MS 8 // chip report period on both halves
#endif
#ifndef SVAL_PTP_REMOTE_POLL_MS
#    define SVAL_PTP_REMOTE_POLL_MS 5 // the other half has not yet said it sends hints
#endif
#ifndef SVAL_PTP_STALE_MS
#    define SVAL_PTP_STALE_MS 50 // no valid sample from a pad with contacts this long: lift them
#endif
// The other half's pad comes over the split link, where one failed
// transaction blocks for the split timeout (20 ms). Its contacts are held at
// their last position this long before they are lifted, so a few failed
// transactions in a row never end a pinch.
#ifndef SVAL_PTP_REMOTE_STALE_MS
#    define SVAL_PTP_REMOTE_STALE_MS 150
#endif
// The other half's pad is fetched on news (its split pointing report carries
// a counter); these bound the gaps when there is none.
#ifndef SVAL_PTP_REMOTE_BACKUP_MS
#    define SVAL_PTP_REMOTE_BACKUP_MS 10 // contacts down or pad not armed
#endif
#ifndef SVAL_PTP_REMOTE_IDLE_MS
#    define SVAL_PTP_REMOTE_IDLE_MS 250 // quiet pad, or a half with no snapshot: heartbeat
#endif
#ifndef SVAL_PTP_MIN_FRAME_US
#    define SVAL_PTP_MIN_FRAME_US 4000 // at most 250 frames a second
#endif
// With contacts on both pads, a frame carrying one pad's new sample waits this
// long for the other pad's, so one frame holds both pads' fresh positions.
#ifndef SVAL_PTP_MERGE_US
#    define SVAL_PTP_MERGE_US (SVAL_PTP_PAD_PERIOD_MS * 1000u)
#endif
#ifndef SVAL_PTP_KEEPALIVE_MS
#    define SVAL_PTP_KEEPALIVE_MS 50 // repeat while down (Linux releases after 100 ms)
#endif
// The host stopped reading the endpoint: reports that could not go out for
// this long stop; the contacts it saw down are released when it reads again.
#ifndef SVAL_PTP_HOST_SILENT_US
#    define SVAL_PTP_HOST_SILENT_US 100000u
#endif
// The palm rejection in force: a contact with more area than this (the chip's
// channels) is a palm, and its Confidence is cleared; 0 is off. On the board
// it is the saved setting (VIA value 58, svalboard.c); host tests set
// SVAL_PTP_PALM_AREA to a number or a variable of their own.
// The touchpad mode: built-in gestures keep the pads on the chip's gestures
// even when the host writes Input Mode 3 (VIA value 59, svalboard.c). Host
// tests set SVAL_PTP_BUILTIN to a number or a variable of their own.
#ifdef SVAL_PTP_BUILTIN
#    define sval_builtin_gestures() ((bool)(SVAL_PTP_BUILTIN))
#elif defined(SVAL_PTP_HOST_TEST)
#    define sval_builtin_gestures() false
#else
bool sval_pad_builtin_gestures(void); // svalboard.c
#    define sval_builtin_gestures() sval_pad_builtin_gestures()
#endif
#ifdef SVAL_PTP_PALM_AREA
#    define sval_palm_area() ((uint8_t)(SVAL_PTP_PALM_AREA))
#elif defined(SVAL_PTP_HOST_TEST)
#    define sval_palm_area() ((uint8_t)0)
#else
uint8_t sval_pad_palm(void); // svalboard.c
#    define sval_palm_area() sval_pad_palm()
#endif
#ifndef SVAL_PTP_REMAP_COUNTS
#    define SVAL_PTP_REMAP_COUNTS 400 // continuity across a stream restart
#endif
#ifndef SVAL_PTP_FLIP_L_X
#    define SVAL_PTP_FLIP_L_X 0
#endif
#ifndef SVAL_PTP_FLIP_L_Y
#    define SVAL_PTP_FLIP_L_Y 0
#endif
#ifndef SVAL_PTP_FLIP_R_X
#    define SVAL_PTP_FLIP_R_X 0
#endif
#ifndef SVAL_PTP_FLIP_R_Y
#    define SVAL_PTP_FLIP_R_Y 0
#endif
#ifndef SVAL_PTP_KEY_BUTTONS
#    define SVAL_PTP_KEY_BUTTONS 0
#endif
#ifndef SVAL_PTP_MOUSE_BEFORE_MODE3
#    define SVAL_PTP_MOUSE_BEFORE_MODE3 1
#endif

#define SVAL_PTP_INPUT_SIZE 35
#define SVAL_PTP_MAX_CONTACTS 5
#define SVAL_PTP_CERT_SIZE 257
#define SVAL_PTP_STATUS_SIZE 24

// --- pads, owner and mouse coordination (multitouch.c, azoteq.c, svalboard.c) ---
// Which hands have a pad (bit 0 left, bit 1 right). A hand without one never
// reports and is never suppressed; with no pad at all nothing is ever sent.
void     sval_ptp_set_pads(uint8_t mask);
void     sval_ptp_set_native(bool native);
void     sval_ptp_set_paused(bool paused);                  // an update is running: no reports
bool     sval_ptp_wants_frames(void);                       // feed pad frames to sval_ptp_feed()
bool     sval_ptp_active(void);                             // NATIVE, a pad, Input Mode 3: the OS owns the pads
bool     sval_ptp_reporting(void);                          // reports are on (mode 3 and the surface switch)
bool     sval_ptp_suppress_mouse(void);                     // the pads' mouse output must be zero
bool     sval_ptp_contacts_down(void);                      // a contact is down or still lifting
uint16_t sval_ptp_take_motion(uint8_t pad);                 // admitted contact motion, counts
bool     sval_ptp_key_button(uint8_t button, bool pressed); // SVAL_PTP_KEY_BUTTONS

// --- Velocity Scroll (keymaps/keymap_support.c, velocity_scroll.h) ---
// While grabbed, the touchpad sends nothing (the OS sees every finger lift) and
// one finger per pad is followed. A finger down at the grab is followed at
// once; after it lifts, only a finger that lands later is.
void     sval_ptp_set_grab(bool grab);
bool     sval_ptp_grab_ready(uint8_t pad); // this pad's contact frames arrive here
// The followed finger: its touch serial (never 0; a new one per finger), and
// its position on the pad (x 0..SVAL_PTP_PAD_X-1, y 0..SVAL_PTP_PAD_Y-1, in the
// touchpad's orientation). 0: none.
uint16_t sval_ptp_grab_finger(uint8_t pad, uint16_t *x, uint16_t *y);

// --- contacts (multitouch.c) ---
// One version-1 56-byte pad frame: pad, flags, mask, seq, size, time, epoch,
// five 8-byte slots (lifetime id, x, y, strength, area). sample_us is when the
// pad was read, on this half's 1 MHz clock.
void    sval_ptp_feed(const uint8_t *frame, uint32_t sample_us);
void    sval_ptp_pad_lost(uint8_t pad);                  // the pad stopped answering, or its half went away
void    sval_ptp_set_stale_ms(uint8_t pad, uint16_t ms); // lift this pad's contacts after this long without a valid sample (0: SVAL_PTP_STALE_MS)
uint8_t sval_ptp_lost_down(uint8_t pad);                 // times a finger on this pad was lifted because its samples stopped (saturates)
bool    sval_ptp_pad_quiet(uint8_t pad);                 // all up, armed, nothing pending
void    sval_ptp_task(void);

// --- contacts for host apps (Sval POINTING_CONTACTS 0x2F, svalboard.c) ---
// The contacts of the last report the computer took, each on its own pad,
// then the fingers down that the touchpad leaves out because more than
// SVAL_PTP_MAX_CONTACTS are down (at their live positions), up to
// SVAL_CONTACTS_REPORT_MAX in all: [0] format 1, [1] bit 0 reporting, bit 1
// left pad in the touchpad, bit 2 right pad, [2] the report's age in ms
// (saturating), [3] contacts n (0..SVAL_CONTACTS_REPORT_MAX), then n x 4
// bytes: [0] bits 0-3 contact ID, bit 4 tip, bit 5 confidence, bit 6 right
// pad, bit 7 the computer does not have this contact (its ID is in a
// separate space); [1..3] x | y << 12, little-endian, x 0..2047 across the
// pad and y 0..1791 down it, in the touchpad's own orientation. A stream that
// stopped (no report for SVAL_PTP_CONTACTS_STALE_MS) reads as no contacts.
// Six: three fingers on each pad. The touchpad itself stays at 5
// (SVAL_PTP_MAX_CONTACTS), the most a Windows Precision Touchpad allows.
#define SVAL_CONTACTS_REPORT_MAX 6
#define SVAL_PTP_CONTACTS_LEN (4 + 4 * SVAL_CONTACTS_REPORT_MAX) // 28
#ifndef SVAL_PTP_CONTACTS_STALE_MS
#    define SVAL_PTP_CONTACTS_STALE_MS (3 * SVAL_PTP_KEEPALIVE_MS)
#endif
// Returns the bytes written (SVAL_PTP_CONTACTS_LEN), or 0 if length is short.
uint8_t sval_ptp_contacts(uint8_t *out, uint8_t length);

// --- status for the dev command and the perf counters (SVAL_PTP_STATUS_SIZE bytes) ---
void sval_ptp_status(uint8_t *out);

// --- feature reports (called from tmk_core, see usb_ptp.h) ---
// report_type: 1 input, 3 feature. Returns the length, or 0 to stall.
uint16_t sval_ptp_get_report(uint8_t report_type, uint8_t report_id, const uint8_t **data);
// Runs after the data stage, so it cannot stall the request any more: a false
// return means the value was ignored, and it is counted (sval_ptp_status).
bool sval_ptp_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data, uint16_t length);
bool sval_ptp_set_report_allowed(uint8_t report_type, uint8_t report_id, uint16_t length);
void sval_ptp_usb_reset(void);
#ifdef SVAL_PTP_HOST_TEST
void sval_ptp_test_reset(void);
#endif
