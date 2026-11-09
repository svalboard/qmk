// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The build-info record (M3): one record in .rodata of every updater build,
// so the image itself says what kind of build it is. make_update.py takes
// fw_version and the version string for the manifest from it (they cannot
// disagree with what the firmware reports) and warns about a manifest whose
// side or pointing device is not the image's (refuses it with --release);
// tools/check_release_elf.py lints it. INFO page 1 returns the version. Version 2 added hand,
// pointing_id and keymap_id; version 3 retired the key fields (byte 6 and
// flags 0x02 and 0x20 are reserved, always 0).

#include <stdint.h>
#include "update_manifest.h"

#define UPDATE_BUILD_INFO_VERSION 3

#define UPDATE_BUILD_RELEASE 0x01         // SVAL_UPDATE_RELEASE
                                          // 0x02 reserved (was: the test key is accepted)
#define UPDATE_BUILD_TEST_HOOKS 0x04      // SVAL_UPDATE_TEST_HOOKS
#define UPDATE_BUILD_KEYTEST 0x08         // SVAL_KEYTEST (never with the updater; listed for the lint)
#define UPDATE_BUILD_HOST_BOOTLOADER 0x10 // SVAL_HOST_BOOTLOADER (Scan Lab reboot into BOOTSEL)
                                          // 0x20 reserved (was: dry-run keys)
#define UPDATE_BUILD_NO_CHORD 0x40        // SVAL_UPDATE_NO_CHORD (dev builds: ARM needs no chord)
#define UPDATE_BUILD_DEV_KEEP 0x80        // SVAL_DEV_KEEP_SETTINGS (dev builds: settings kept across flashes)

typedef struct __attribute__((packed)) {
    char     magic[4];       // "SVBI"
    uint8_t  info_ver;       // UPDATE_BUILD_INFO_VERSION
    uint8_t  flags;          // UPDATE_BUILD_*
    uint8_t  reserved0;      // 0
    uint8_t  updater_proto;  // UPDATE_PROTOCOL_VERSION
    uint32_t fw_version;     // SVAL_FW_VERSION (D17, D32)
    char     version[UPDATE_VERSION_CHARS]; // SVAL_FW_VERSION_STRING, NUL-padded (the release tag)
    uint8_t  hand;           // UPDATE_HAND_* of this build (update_self.h); 0xFF in host tests
    uint8_t  pointing_id;    // UPDATE_POINTING_* of this build; 0xFF in host tests
    uint8_t  keymap_id;      // SVAL_UPDATE_KEYMAP_ID (updater/updater.mk): 1 sval, 2 blank, 0 any other keymap
    uint8_t  reserved;       // 0
} update_build_info_t;       // 32 B

extern const update_build_info_t sval_update_build_info;
