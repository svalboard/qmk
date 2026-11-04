// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad (experiment/native-ptp): HID report descriptor.
//
// Plain bytes, so tmk_core (usb_descriptor.c), the host tests and
// tools/multitouch/ptp_desc_check.py all read the same thing. Shape follows
// Microsoft's sample Precision Touchpad descriptor and George Norton's
// per-finger macro, with the certification blob on vendor page 0xFF00.
#pragma once

#ifndef SVAL_PTP_GAP_MM
#    define SVAL_PTP_GAP_MM 10
#endif
// 12 at most: seven times the pads' width must fit the descriptor's signed
// 16-bit maximum (below).
#if SVAL_PTP_GAP_MM < 0 || SVAL_PTP_GAP_MM > 12
#    error "SVAL_PTP_GAP_MM must be 0..12"
#endif

// One pad: X 0..2047 over 43 mm, Y 0..1791 over 40 mm.
#define SVAL_PTP_PAD_X 2048
#define SVAL_PTP_PAD_Y 1792
// Gap in counts, rounded: G = round(gap_mm * 2048 / 43).
#define SVAL_PTP_GAP ((SVAL_PTP_GAP_MM * 2048 * 2 + 43) / (2 * 43))
#define SVAL_PTP_PAD_STRIDE (SVAL_PTP_PAD_X + SVAL_PTP_GAP)
#define SVAL_PTP_XMAX (2 * SVAL_PTP_PAD_X - 1 + SVAL_PTP_GAP)
#define SVAL_PTP_YMAX (SVAL_PTP_PAD_Y - 1)
// Physical size in 0.1 mm.
#define SVAL_PTP_PXMAX (860 + 10 * SVAL_PTP_GAP_MM)
#define SVAL_PTP_PYMAX 400

#define SVAL_PTP_REPORT_ID_INPUT 1
#define SVAL_PTP_REPORT_ID_CAPS 2
#define SVAL_PTP_REPORT_ID_CERT 3
#define SVAL_PTP_REPORT_ID_LATENCY 4
#define SVAL_PTP_REPORT_ID_MODE 5
#define SVAL_PTP_REPORT_ID_SWITCHES 6

#define SVAL_PTP_LE16(v) ((v)&0xFF), (((v) >> 8) & 0xFF)

// 71 bytes per finger: Confidence, Tip, 6 pad bits, Contact ID, X, Y.
#define SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM)                                                        \
    0x05, 0x0D,                              /* Usage Page (Digitizers)         */ \
        0x09, 0x22,                          /* Usage (Finger)                  */ \
        0xA1, 0x02,                          /* Collection (Logical)            */ \
        0x15, 0x00, 0x25, 0x01,              /*   Logical 0..1                  */ \
        0x75, 0x01, 0x95, 0x02,              /*   2 x 1 bit                     */ \
        0x09, 0x47,                          /*   Usage (Confidence)    bit 0   */ \
        0x09, 0x42,                          /*   Usage (Tip Switch)    bit 1   */ \
        0x81, 0x02,                          /*   Input (Data,Var,Abs)          */ \
        0x95, 0x06, 0x81, 0x03,              /*   6 bits padding                */ \
        0x25, 0x0F,                          /*   Logical max 15                */ \
        0x75, 0x08, 0x95, 0x01,              /*   1 x 8 bits                    */ \
        0x09, 0x51,                          /*   Usage (Contact Identifier)    */ \
        0x81, 0x02,                          /*   Input                         */ \
        0x05, 0x01,                          /*   Usage Page (Generic Desktop)  */ \
        0x55, 0x0E,                          /*   Unit Exponent -2              */ \
        0x65, 0x11,                          /*   Unit (SI Linear, cm)          */ \
        0x35, 0x00,                          /*   Physical min 0                */ \
        0x75, 0x10,                          /*   16 bits                       */ \
        0x26, SVAL_PTP_LE16(XM),  /*   Logical max X             */ \
        0x46, SVAL_PTP_LE16(PXM), /*   Physical max X                */ \
        0x09, 0x30,                          /*   Usage (X)                     */ \
        0x81, 0x02,                          /*   Input                         */ \
        0x26, SVAL_PTP_LE16(YM),  /*   Logical max Y             */ \
        0x46, SVAL_PTP_LE16(PYM), /*   Physical max Y                */ \
        0x09, 0x31,                          /*   Usage (Y)                     */ \
        0x81, 0x02,                          /*   Input                         */ \
        0x55, 0x00, 0x65, 0x00, 0x45, 0x00,  /*   reset exponent, unit, phys    */ \
        0xC0                                 /* End Collection                  */

#define SVAL_PTP_REPORT_DESCRIPTOR_SIZED(XM, PXM, YM, PYM)                                                                                                         \
    0x05, 0x0D,                                                                                          /* Usage Page (Digitizers)         */ \
        0x09, 0x05,                                                                                      /* Usage (Touch Pad)               */ \
        0xA1, 0x01,                                                                                      /* Collection (Application)        */ \
        0x85, SVAL_PTP_REPORT_ID_INPUT,                                                                  /*   Report ID 1 (input)           */ \
        SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM), SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM), SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM), SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM), SVAL_PTP_FINGER_SIZED(XM, PXM, YM, PYM), 0x05, 0x0D, /*   Usage Page (Digitizers)       */ \
        0x55, 0x0C,                                                                                      /*   Unit Exponent -4              */ \
        0x66, 0x01, 0x10,                                                                                /*   Unit (SI Linear, seconds)     */ \
        0x47, 0xFF, 0xFF, 0x00, 0x00,                                                                    /*   Physical max 65535            */ \
        0x27, 0xFF, 0xFF, 0x00, 0x00,                                                                    /*   Logical max 65535             */ \
        0x75, 0x10, 0x95, 0x01,                                                                          /*   1 x 16 bits                   */ \
        0x09, 0x56,                                                                                      /*   Usage (Scan Time)             */ \
        0x81, 0x02,                                                                                      /*   Input                         */ \
        0x55, 0x00, 0x65, 0x00, 0x45, 0x00,                                                              /*   reset exponent, unit, phys    */ \
        0x09, 0x54,                                                                                      /*   Usage (Contact Count)         */ \
        0x25, 0x05,                                                                                      /*   Logical max 5                 */ \
        0x75, 0x08, 0x95, 0x01,                                                                          /*   1 x 8 bits                    */ \
        0x81, 0x02,                                                                                      /*   Input                         */ \
        0x05, 0x09,                                                                                      /*   Usage Page (Button)           */ \
        0x19, 0x01, 0x29, 0x03,                                                                          /*   Buttons 1..3                  */ \
        0x25, 0x01, 0x75, 0x01, 0x95, 0x03,                                                              /*   3 x 1 bit, 0..1               */ \
        0x81, 0x02,                                                                                      /*   Input                         */ \
        0x95, 0x05, 0x81, 0x03,                                                                          /*   5 bits padding                */ \
        0x05, 0x0D,                                                                                      /*   Usage Page (Digitizers)       */ \
        0x85, SVAL_PTP_REPORT_ID_CAPS,                                                                   /*   Report ID 2 (capabilities)    */ \
        0x09, 0x55,                                                                                      /*   Contact Count Maximum (low)   */ \
        0x09, 0x59,                                                                                      /*   Pad Type (high nibble)        */ \
        0x75, 0x04, 0x95, 0x02, 0x25, 0x0F,                                                              /*   2 x 4 bits                    */ \
        0xB1, 0x02,                                                                                      /*   Feature                       */ \
        0x06, 0x00, 0xFF,                                                                                /*   Usage Page (Vendor 0xFF00)    */ \
        0x85, SVAL_PTP_REPORT_ID_CERT,                                                                   /*   Report ID 3 (certification)   */ \
        0x09, 0xC5,                                                                                      /*   Usage (0xC5)                  */ \
        0x15, 0x00, 0x26, 0xFF, 0x00,                                                                    /*   Logical 0..255                */ \
        0x75, 0x08, 0x96, 0x00, 0x01,                                                                    /*   256 x 8 bits                  */ \
        0xB1, 0x02,                                                                                      /*   Feature                       */ \
        0x05, 0x0D,                                                                                      /*   Usage Page (Digitizers)       */ \
        0x85, SVAL_PTP_REPORT_ID_LATENCY,                                                                /*   Report ID 4 (latency mode)    */ \
        0x09, 0x60,                                                                                      /*   Usage (Latency Mode)          */ \
        0x25, 0x01, 0x75, 0x01, 0x95, 0x01,                                                              /*   1 bit                         */ \
        0xB1, 0x02,                                                                                      /*   Feature                       */ \
        0x95, 0x07, 0xB1, 0x03,                                                                          /*   7 bits padding                */ \
        0xC0,                                                                                            /* End Collection (Touch Pad)      */ \
        0x05, 0x0D,                                                                                      /* Usage Page (Digitizers)         */ \
        0x09, 0x0E,                                                                                      /* Usage (Device Configuration)    */ \
        0xA1, 0x01,                                                                                      /* Collection (Application)        */ \
        0x85, SVAL_PTP_REPORT_ID_MODE,                                                                   /*   Report ID 5 (input mode)      */ \
        0x09, 0x22,                                                                                      /*   Usage (Finger)                */ \
        0xA1, 0x02,                                                                                      /*   Collection (Logical)          */ \
        0x09, 0x52,                                                                                      /*     Usage (Input Mode)          */ \
        0x25, 0x0A, 0x75, 0x08, 0x95, 0x01,                                                              /*     1 x 8 bits, 0..10           */ \
        0xB1, 0x02,                                                                                      /*     Feature                     */ \
        0xC0,                                                                                            /*   End Collection                */ \
        0x09, 0x22,                                                                                      /*   Usage (Finger)                */ \
        0xA1, 0x00,                                                                                      /*   Collection (Physical)         */ \
        0x85, SVAL_PTP_REPORT_ID_SWITCHES,                                                               /*     Report ID 6 (switches)      */ \
        0x09, 0x57,                                                                                      /*     Usage (Surface Switch)      */ \
        0x09, 0x58,                                                                                      /*     Usage (Button Switch)       */ \
        0x75, 0x01, 0x95, 0x02, 0x25, 0x01,                                                              /*     2 x 1 bit                   */ \
        0xB1, 0x02,                                                                                      /*     Feature                     */ \
        0x95, 0x06, 0xB1, 0x03,                                                                          /*     6 bits padding              */ \
        0xC0,                                                                                            /*   End Collection                */ \
        0xC0                                                                                             /* End Collection (Configuration)  */

// The surface the OS sees: seven times the pads' area (both pads and the gap)
// on each axis, at the same counts per mm, so pointer speed and the OS's
// gesture distances are those of the pads, with the pads in its center, three
// areas in from each edge. Sniper and boost scale the contacts' travel
// (ptp.c), and the room around the pads lets a boosted stroke travel far
// before it meets an edge. 7 x 4572 = 32004 counts: the largest odd multiple
// under the descriptor's signed 16-bit logical maximum (32767). Host tests
// build with SVAL_PTP_SURFACE_TIMES 1 (the pads' area alone) as well.
#ifndef SVAL_PTP_SURFACE_TIMES
#    define SVAL_PTP_SURFACE_TIMES 7
#endif
#if SVAL_PTP_SURFACE_TIMES < 1 || (SVAL_PTP_SURFACE_TIMES % 2) == 0
#    error "SVAL_PTP_SURFACE_TIMES must be odd: the pads sit in the center"
#endif
#define SVAL_PTP_OS_XOFF (((SVAL_PTP_SURFACE_TIMES - 1) / 2) * (SVAL_PTP_XMAX + 1))
#define SVAL_PTP_OS_YOFF (((SVAL_PTP_SURFACE_TIMES - 1) / 2) * (SVAL_PTP_YMAX + 1))
#define SVAL_PTP_OS_XMAX (SVAL_PTP_SURFACE_TIMES * (SVAL_PTP_XMAX + 1) - 1)
#define SVAL_PTP_OS_YMAX (SVAL_PTP_SURFACE_TIMES * (SVAL_PTP_YMAX + 1) - 1)
#define SVAL_PTP_OS_PXMAX (SVAL_PTP_SURFACE_TIMES * SVAL_PTP_PXMAX)
#define SVAL_PTP_OS_PYMAX (SVAL_PTP_SURFACE_TIMES * SVAL_PTP_PYMAX)
#if SVAL_PTP_OS_XMAX > 32767 || SVAL_PTP_OS_PXMAX > 32767
#    error "the surface must fit a signed 16-bit logical and physical maximum"
#endif
#define SVAL_PTP_FINGER SVAL_PTP_FINGER_SIZED(SVAL_PTP_OS_XMAX, SVAL_PTP_OS_PXMAX, SVAL_PTP_OS_YMAX, SVAL_PTP_OS_PYMAX)
#define SVAL_PTP_REPORT_DESCRIPTOR SVAL_PTP_REPORT_DESCRIPTOR_SIZED(SVAL_PTP_OS_XMAX, SVAL_PTP_OS_PXMAX, SVAL_PTP_OS_YMAX, SVAL_PTP_OS_PYMAX)
