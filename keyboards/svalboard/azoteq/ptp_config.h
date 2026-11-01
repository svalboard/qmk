// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad build settings (SVAL_NATIVE_PTP, azoteq/multitouch.mk).
// Included by azoteq/config.h and pointing_auto/config.h. The pad geometry is
// fixed at build time because the host reads it once, at connect.
#pragma once

#ifdef SVAL_NATIVE_PTP
#    define SVAL_TOUCH_OWNER_DEFAULT 3 // NATIVE (ptp.h); the dev command can switch it
#    define SVAL_PTP_GAP_MM 10         // unreachable surface between the pads, 0..50
#    define SVAL_PTP_FLIP_L_X 0
#    define SVAL_PTP_FLIP_L_Y 0
#    define SVAL_PTP_FLIP_R_X 0
#    define SVAL_PTP_FLIP_R_Y 0
#    define SVAL_PTP_PAD_PERIOD_MS 8     // chip report period on both halves
#    define SVAL_PTP_REMOTE_POLL_MS 5    // the other half has not yet said it sends hints
#    define SVAL_PTP_STALE_MS 50         // a pad with contacts silent this long: lift them
#    define SVAL_PTP_REMOTE_STALE_MS 150 // the same for the other half's pad: rides out failed split transactions
#    define SVAL_PTP_REMOTE_BACKUP_MS 10 // the other pad has contacts: fetch at least this often
#    define SVAL_PTP_REMOTE_IDLE_MS 250  // the other pad is quiet: heartbeat (news is fetched at once)
#    define SVAL_PTP_MIN_FRAME_US 4000   // at most 250 frames a second
#    define SVAL_PTP_KEEPALIVE_MS 50     // repeat while down (Linux releases after 100 ms)
#    ifndef SVAL_PTP_KEY_BUTTONS
#        define SVAL_PTP_KEY_BUTTONS 0 // 1: MS_BTN1/2 are touchpad Buttons 2/3 while it reports
#    endif
#    ifndef SVAL_PTP_MOUSE_BEFORE_MODE3
#        define SVAL_PTP_MOUSE_BEFORE_MODE3 1 // 0: pads silent until the host writes mode 3
#    endif
// Both halves run their pad at this period. The chip's own gestures stay on:
// macOS, a BIOS or any host that never writes Input Mode 3 gets today's
// touchpad (tap, scroll, drag), and in mode 3 the chip's mouse output is
// suppressed, so its gestures cannot reach the host there.
#    define AZOTEQ_IQS5XX_REPORT_RATE SVAL_PTP_PAD_PERIOD_MS
// The configuration descriptor gains an interface, and Windows caches a
// device's interface set by VID, PID and bcdDevice: a new bcdDevice makes it
// read the new set instead of binding the old one. 0x0F03 has never been
// used by a build without the touchpad interface.
#    undef DEVICE_VER
#    define DEVICE_VER 0x0F03
#endif
