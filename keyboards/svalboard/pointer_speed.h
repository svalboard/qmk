// SPDX-License-Identifier: GPL-2.0-or-later
// Pointer speed per module: how far the DPI list goes for each pointing module,
// and a TrackPoint's software speed.
//
// A trackball sets its sensor's DPI. A touchpad's pointer stops gaining at its
// resolution (scroll_speed.h), so the list stops there. A TrackPoint has no DPI
// to set: its list is a software multiplier, DPI over 800 (800 is 1:1, the
// motion it has always had; 400 is half, 1600 is double). Every hand's DPI
// index is saved, so the list only ever grows at its end.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The DPI each module's list ends at.
#define SVAL_DPI_MAX_PMW3360 12000
#define SVAL_DPI_MAX_PMW3389 16000
#define SVAL_DPI_MAX_TRACKPOINT 3200 // 4x its own motion; past that its counts only skip
#define SVAL_TRACKPOINT_BASE_DPI 800 // 1:1

enum sval_dpi_module {
    SVAL_DPI_MODULE_ANY = 0, // not known yet: the whole list
    SVAL_DPI_MODULE_PMW3360,
    SVAL_DPI_MODULE_PMW3389,
    SVAL_DPI_MODULE_PAD,
    SVAL_DPI_MODULE_TRACKPOINT,
};

// The biggest DPI a module's list reaches (0xFFFF for any).
uint16_t sval_dpi_module_max(uint8_t module);
// The highest list index a module's DPI step keys and writes reach.
uint8_t sval_dpi_index_limit(const uint16_t *choices, uint8_t n, uint8_t module);
// An index held to that limit; an index within it stays.
uint8_t sval_dpi_index_clamp(const uint16_t *choices, uint8_t n, uint8_t module, uint8_t index);
// The index a TrackPoint's list starts from: 800, its motion as it has always been.
uint8_t sval_trackpoint_base_index(const uint16_t *choices, uint8_t n);

// ---------------------------------------------------------------- TrackPoint scaler
typedef struct {
    int32_t rem_x, rem_y; // the part of a count not yet sent, in 1/800 units
    uint16_t cpi;         // the speed the remainders belong to
} sval_tp_scale_t;

// Raw counts in, scaled counts out at cpi / 800. 0 and 800 pass the counts
// through unchanged (the default is exactly the old motion). The part of a
// count that doesn't make a whole one carries to the next report, toward zero,
// so slow pushes at a low speed are not lost.
void sval_tp_scale(sval_tp_scale_t *s, uint16_t cpi, int32_t x, int32_t y, int32_t *out_x, int32_t *out_y);
