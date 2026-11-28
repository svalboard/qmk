// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "test_common.h"
// The module's core hook switches, as modules/svalboard/core/post_config.h sets them.
#include "../../modules/svalboard/core/sval_runtime_settings.h"
#define TAPPING_TERM_PER_KEY
// As the generated sval_config.h sets it.
#define SVAL_TAP_DANCE_ENTRIES 8
// TAP_DANCE_MAX_SIMULTANEOUS comes from keyboards/svalboard/config.h (test.mk).
