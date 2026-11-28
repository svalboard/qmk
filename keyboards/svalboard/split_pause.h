// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Split pause (D15, docs/updater-plan.md M2), SVAL_UPDATER builds only.
//
// While paused, the half with USB skips the whole split exchange
// (matrix_post_scan: matrix, layers, RGB sync, pointing, watchdog ping), and
// the presence ping and the Scan Lab relay hold off too; only the updater's
// own KEYBOARD_UPDATE RPCs cross the link. The other half's keys are released
// once on entry. On resume the split watchdog is re-armed, so a half that
// rebooted meanwhile is pinged again. The local half keeps scanning, and its
// keys keep working throughout.
//
// split_pause.c overrides QMK's weak matrix_scan() (quantum/matrix_common.c)
// to do this. The resume also restarts the split transport's count of failed
// scans (transport_connection_restart, a Svalboard patch to
// quantum/split_common/split_util.c).

#include <stdbool.h>

// Pause (on) or resume (off) the split link. Only the half with USB pauses,
// only while the link is up, and only one owner at a time: true if this call
// paused the link (on), false if it was refused, including when the link is
// already paused (someone else owns that pause; only the caller that got true
// resumes). Resume (off) returns true; resuming an unpaused link does nothing.
// The pause is off at boot (bootmagic scans the matrix before init).
bool sval_split_pause(bool on);
bool sval_split_paused(void);
