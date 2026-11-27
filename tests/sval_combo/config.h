// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "test_common.h"
// The module's core hook switches, as modules/svalboard/core/post_config.h sets them.
#include "../../modules/svalboard/core/sval_runtime_settings.h"
// As post_config.h: per-combo terms come from get_combo_term() in sval_combo.c.
#define COMBO_TERM_PER_COMBO
// As the generated sval_config.h sets it.
#define SVAL_COMBO_ENTRIES 4
