// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// This build's side and pointing device (D18, R8): the image check compares
// builds, it does not detect hardware. update_image.c checks manifests against
// them, and update_build_info.c records them in the build-info record so
// make_update.py can cross-check a manifest against the image itself (M3).

#include "update_manifest.h"

#ifndef SVAL_UPDATER_HOST_TEST
#    if defined(SVAL_EITHER_HAND)
#        if defined(INIT_EE_HANDS_LEFT) || defined(INIT_EE_HANDS_RIGHT)
#            error "updater: an either-hand build sets neither INIT_EE_HANDS_LEFT nor INIT_EE_HANDS_RIGHT"
#        endif
// The image is for both halves; the half it runs on is the side it learned at boot.
#        include "side.h"
#        define UPDATE_SELF_HAND UPDATE_HAND_EITHER
#        define UPDATE_RUNNING_HAND sval_side()
#    elif defined(INIT_EE_HANDS_LEFT) && !defined(INIT_EE_HANDS_RIGHT)
#        define UPDATE_SELF_HAND UPDATE_HAND_LEFT
#    elif defined(INIT_EE_HANDS_RIGHT) && !defined(INIT_EE_HANDS_LEFT)
#        define UPDATE_SELF_HAND UPDATE_HAND_RIGHT
#    else
#        error "updater: the build must set exactly one of INIT_EE_HANDS_LEFT / INIT_EE_HANDS_RIGHT"
#    endif
#    ifndef UPDATE_RUNNING_HAND
#        define UPDATE_RUNNING_HAND UPDATE_SELF_HAND
#    endif

#    if defined(SVAL_POINTING_AUTO)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_AUTO
#    elif defined(POINTING_DEVICE_IS_PMW3389) + defined(POINTING_DEVICE_IS_PMW3360) + defined(PS2_ENABLE) + defined(AZOTEQ_IQS5XX_TPS43) > 1
#        error "updater: more than one pointing device in this build"
#    elif defined(POINTING_DEVICE_IS_PMW3389)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_PMW3389
#    elif defined(POINTING_DEVICE_IS_PMW3360)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_PMW3360
#    elif defined(PS2_ENABLE)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_TRACKPOINT
#    elif defined(AZOTEQ_IQS5XX_TPS43)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_AZOTEQ
#    else
#        define UPDATE_SELF_POINTING UPDATE_POINTING_NONE
#    endif
#endif
