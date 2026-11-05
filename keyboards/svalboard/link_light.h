// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// The link-loss light (docs/link-light.md): a half that was paired with the
// other half in this power session, and then lost it without being told to
// stop, blinks amber until the pairing comes back.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The pairing came up (or back): arms the light and clears a loss.
void sval_link_paired(void);
// The pairing was lost: the light shows, if a pairing came up since boot.
void sval_link_lost(void);
// The pairing came back after a loss (svalboard.c): show the current lighting
// on both halves at once.
void sval_link_recovered(void);
// Told to stop on purpose (a restart, the bootloader): no light, and nothing
// armed until paired again. The half with USB pairs again only once the link has
// gone down and up, or after SVAL_LINK_REARM_MS if it never went down.
void sval_link_stopped(void);

// Every housekeeping pass, on both halves: the split cable's own pairing.
// The half with USB pairs on the first presence ping the other half answers,
// and loses it when the split transport counts as disconnected (the 200 ms
// check, SPLIT_MIN_DISCONNECT_MS). The other half pairs on the first presence
// ping it gets, and loses it when pings stop for SVAL_LINK_SUB_LOST_MS.
void sval_link_task(void);
// The presence ping's link_quiet byte (svalboard.h).
#define SVAL_LINK_QUIET_SUSPEND 1  // the half with USB is in USB suspend
#define SVAL_LINK_QUIET_STOPPING 2 // its last ping before it restarts on purpose

// The other half: a presence ping came in. quiet: the half with USB is in USB
// suspend (its own light is dark then, and so is this half's).
void sval_link_ping_seen(bool quiet);
// The half with USB: the presence ping's quiet byte.
bool sval_link_quiet(void);

// The light is due now (updater.c shows it when the updater shows nothing):
// lost, and not asleep (USB suspend here, or in the half with USB's last ping).
bool sval_link_light(void);

#ifdef SVAL_LINK_TEST
// Dev builds only (SVAL_LINK_TEST=yes, never in a release): raw HID protocol
// 0xFA in the client wrapper, and its split RPC on the other half.
#    define SVAL_LINK_TEST_PROTO 0xFA
void sval_link_test_command(uint8_t *data, uint8_t length);
bool sval_link_test_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
void sval_link_test_task(void);
#endif
