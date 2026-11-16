// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Every record in the settings area (settings_area.h), one line each:
//   X(NAME, id, capacity in bytes)
// To add a setting: a new line with the next unused id (ids are never reused,
// even after a setting is dropped) and its payload size. The build fails if
// the area would overflow; tools/pre_pr.sh warns at half full.
// A record may later grow: append fields at the end and raise its capacity.
// A record stored by older firmware is then shorter, and the new fields keep
// their defaults (settings_area_get() reports the stored length).
#pragma once

#define SVAL_SETTINGS_RECORDS(X)       \
    X(SVAL_SET_POINTER, 1, 8)          \
    X(SVAL_SET_LAYER_COLORS, 2, 48)    \
    X(SVAL_SET_SCAN, 3, 16)            \
    X(SVAL_SET_PAD_GESTURES, 4, 2)     \
    X(SVAL_SET_PAD_TUNING, 5, 4)       \
    X(SVAL_SET_PAD_PALM, 6, 2)         \
    X(SVAL_SET_PAD_MODE, 7, 2)         \
    X(SVAL_SET_SCROLL_SPEED, 8, 5)     \
    X(SVAL_SET_AUTOMOUSE_LAYERS, 9, 4)

#define SVAL_SETTINGS_ID_(name, id, cap) name = id,
enum sval_setting_id { SVAL_SETTINGS_RECORDS(SVAL_SETTINGS_ID_) };
#undef SVAL_SETTINGS_ID_

#define SVAL_SETTINGS_CAP_(name, id, cap) name##_CAP = cap,
enum sval_setting_cap { SVAL_SETTINGS_RECORDS(SVAL_SETTINGS_CAP_) };
#undef SVAL_SETTINGS_CAP_
