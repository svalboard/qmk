// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// In-firmware updater: the wire format shared by the firmware, the host tools
// (kb/tools/make_update.py, kb/tools/sval_update.py) and Keybard. Everything
// here is protocol: change a value only together with UPDATE_PROTOCOL_VERSION
// and the tools. Plan: docs/updater-plan.md (M1).

#include <stdint.h>

// 4: protocol 3 plus one approval for both halves (PAIR_MANIFEST, the pair
// ARM and the held approval, D41), SETTINGS_RESET (D42), the ARM reply flags,
// STATUS byte 23 and INFO flags bit7 (no-chord dev builds, D43). Report and
// piece sizes, the manifest and the relay's PAGE path are unchanged; the split
// link gains one op (USPLIT_OP_SETTINGS_RESET).
// 3: protocol 2 in 64-byte raw HID reports, with larger MANIFEST and CHUNK
// pieces and the piece sizes in INFO (updater.c). The manifest is unchanged
// (UPDATE_MANIFEST_VERSION 2), and so is the relay over the split cable.
// 2: the manifest travels alone, 108 bytes, with nothing after it. Version 1
// (never released) put a 64-byte signature after it.
#define UPDATE_PROTOCOL_VERSION 4 // reported by INFO, and stored as updater_proto in images

// VIA custom-value channel ('U'). The others: IDENTITY 0x49, SCANLAB 0x53, KEYTEST 0x54.
#define UPDATE_CHANNEL 0x55

// ---- manifest ------------------------------------------------------------------

#define UPDATE_MANIFEST_MAGIC 0x50555653u // "SVUP" as bytes in flash/wire order
#define UPDATE_MANIFEST_VERSION 2         // version 1 manifests are refused (UNSUPPORTED)
#define UPDATE_SHA512_BYTES 64
#define UPDATE_VERSION_CHARS 16

// hand
#define UPDATE_HAND_LEFT 0
#define UPDATE_HAND_RIGHT 1
// Manifests and build info only: one image for both halves (SVAL_EITHER_HAND),
// which learns its side at boot. A running half reports 0 or 1, or 0xFF while
// it has no side (it is then addressed with hand 0xFF, this half).
#define UPDATE_HAND_EITHER 2

// pointing_id: this half's pointing device only (D18). Append-only. Reported, and
// warned about by the host tools, but never checked by the firmware (D33).
#define UPDATE_POINTING_NONE 0
#define UPDATE_POINTING_TRACKPOINT 1
#define UPDATE_POINTING_PMW3360 2
#define UPDATE_POINTING_PMW3389 3
#define UPDATE_POINTING_AZOTEQ 4
#define UPDATE_POINTING_AUTO 5 // one build with every driver, started for whichever module answers

// keymap_id: display only, never checked (D18).
#define UPDATE_KEYMAP_UNKNOWN 0
#define UPDATE_KEYMAP_SVAL 1
#define UPDATE_KEYMAP_BLANK 2

// flags
#define UPDATE_FLAG_RELEASE 0x0001u
#define UPDATE_FLAG_DIAGNOSTIC 0x0002u // scanlab / keytest / host-bootloader images
#define UPDATE_FLAGS_KNOWN (UPDATE_FLAG_RELEASE | UPDATE_FLAG_DIAGNOSTIC)

// image_len rules
#define UPDATE_IMAGE_MIN 0x200u   // boot2 page, vector table and at least some code
#define UPDATE_IMAGE_ALIGN 0x100u // one flash page

typedef struct __attribute__((packed)) {
    uint32_t magic;          // UPDATE_MANIFEST_MAGIC
    uint16_t manifest_ver;   // UPDATE_MANIFEST_VERSION
    uint8_t  reserved0;      // must be 0
    uint8_t  hand;          // 0 left, 1 right
    uint8_t  pointing_id;    // this half's pointing device only (D18): UPDATE_POINTING_*
    uint8_t  keymap_id;      // 1 sval, 2 blank: display only, never checked (D18)
    uint16_t flags;          // bit0 RELEASE, bit1 DIAGNOSTIC; other bits must be 0
    uint16_t security_epoch; // refuse < device epoch (D19)
    uint8_t  storage_format; // refuse < current settings format (D19)
    uint8_t  updater_proto;  // updater protocol of the image
    uint32_t image_len;      // UPDATE_IMAGE_MIN..SVAL_UPDATE_MAX_IMAGE, multiple of 256
    uint32_t fw_version;     // numeric, also used in split presence (D17)
    uint32_t reserved;       // must be 0 (pads the manifest to the plan's 108 bytes)
    char     version[UPDATE_VERSION_CHARS]; // NUL-padded display string
    uint8_t  sha512[UPDATE_SHA512_BYTES];   // over image_len bytes
} sval_update_manifest_t; // a .svup file is these bytes, then the image

#define UPDATE_MANIFEST_BYTES 108 // also the MANIFEST op's buffer and the relay's BEGIN
_Static_assert(sizeof(sval_update_manifest_t) == UPDATE_MANIFEST_BYTES, "manifest is 108 bytes on the wire");
_Static_assert(__builtin_offsetof(sval_update_manifest_t, image_len) == 16, "manifest layout");
_Static_assert(__builtin_offsetof(sval_update_manifest_t, sha512) == 44, "manifest layout");

// ---- ops (the VIA value_id byte; see updater.c for the request layouts) ----------

typedef enum {
    UPDATE_OP_INFO      = 0x00,
    UPDATE_OP_MANIFEST  = 0x01,
    UPDATE_OP_ARM       = 0x02,
    UPDATE_OP_BEGIN     = 0x03,
    UPDATE_OP_CHUNK     = 0x04,
    UPDATE_OP_END       = 0x05,
    UPDATE_OP_STATUS    = 0x06,
    UPDATE_OP_COMMIT    = 0x07,
    UPDATE_OP_ABORT     = 0x08,
    UPDATE_OP_REBIND    = 0x09,
    UPDATE_OP_TEST_HALT = 0x0A, // SVAL_UPDATE_TEST_HOOKS builds only
    UPDATE_OP_DIAG      = 0x0B, // measurements for the hardware tests (M1 #13)
    UPDATE_OP_RELAY     = 0x0C, // M2: progress of a relay to the other half
    UPDATE_OP_PAIR_MANIFEST  = 0x0D, // protocol 4: this half's manifest, so one chord approves both (D41)
    UPDATE_OP_SETTINGS_RESET = 0x0E, // protocol 4: every stored setting back to defaults, both halves (D42)
} update_op_t;

// ARM request mode (req[1], protocol 4; a protocol 3 host sends 0).
#define UPDATE_ARM_MODE_SINGLE 0
#define UPDATE_ARM_MODE_PAIR 1

// ARM reply flags (rsp[13], protocol 4).
#define UPDATE_ARM_PRE_APPROVED 0x01 // BEGIN is accepted with no chord, and no blue light is shown
#define UPDATE_ARM_BY_PAIR 0x02      // pre-approved by the held approval of a pair ARM (D41)
#define UPDATE_ARM_NO_CHORD 0x04     // pre-approved because this is a no-chord dev build (D43)
#define UPDATE_ARM_PAIR 0x08         // a pair ARM: the chord approves both manifests

// ---- status codes ------------------------------------------------------------------
// The first three keep the values of the sibling sets (identity_status_t: OK 0,
// INVALID 1, UNAVAILABLE 2; FLASH_ERR takes IDENTITY_WRITE_FAILED's 3).

typedef enum {
    UPDATE_OK            = 0,
    UPDATE_INVALID       = 1,  // malformed request, wrong state, or unwrapped packet
    UPDATE_UNAVAILABLE   = 2,  // flash is not 16 MiB (D26), or settings writes are failing (D12)
    UPDATE_FLASH_ERR     = 3,  // erase or program did not read back
    UPDATE_ACCEPTED      = 4,  // slow work started; poll STATUS
    UPDATE_BUSY          = 5,
    UPDATE_NOT_CONFIRMED = 6,  // the gesture was not made in time
    UPDATE_WRONG_HW      = 7,  // retired: the pointing device is not checked (D33); never returned, kept so codes don't shift
    UPDATE_BAD_SIG       = 8,  // retired: protocol 1's signature check; never returned, kept so codes don't shift
    UPDATE_BAD_HASH      = 9,  // staged image does not match the manifest's SHA-512
    UPDATE_BAD_IMAGE     = 10, // structure checks failed (boot2, SP, reset vector, length, flags)
    UPDATE_EPOCH         = 11, // security epoch below this device's floor
    UPDATE_OUT_OF_ORDER  = 12,
    UPDATE_OVERRUN       = 13,
    UPDATE_TOO_LARGE     = 14, // image_len above SVAL_UPDATE_MAX_IMAGE
    UPDATE_TIMEOUT       = 15,
    UPDATE_OTHER_CLIENT  = 16,
    UPDATE_UNSUPPORTED   = 17, // the request's hand is neither half's, or a manifest/protocol version this build doesn't know
    UPDATE_STORAGE       = 18, // settings storage format below this device's floor
} update_status_t;

// ---- states (reported by INFO and STATUS) -------------------------------------------

typedef enum {
    UPDATE_STATE_IDLE               = 0,
    UPDATE_STATE_MANIFEST_LOADING   = 1,
    UPDATE_STATE_CONFIRM_WAIT       = 2,
    UPDATE_STATE_VERIFYING_MANIFEST = 3,
    UPDATE_STATE_ERASING            = 4,
    UPDATE_STATE_RECEIVING          = 5,
    UPDATE_STATE_VERIFYING_IMAGE    = 6,
    UPDATE_STATE_VERIFIED           = 7,
    UPDATE_STATE_COMMITTING         = 8,
    UPDATE_STATE_ERROR              = 9,
    // M2, a session for the other half (hand = the other side), on the half
    // with USB only. The image is staged and verified here first (states 1-7),
    // then:
    UPDATE_STATE_RELAYING           = 10, // COMMIT accepted: relaying to the other half, which checks it itself
    UPDATE_STATE_RELAYED            = 11, // the other half holds the image, verified; waiting for COMMIT
    UPDATE_STATE_SUBSIDE_COMMITTING = 12, // the other half is committing; the link stays paused (R18)
} update_state_t;

// ---- the other half, as split presence shows it (M2, D17, V) -------------------------
// Reported by STATUS (byte 22) on the half with USB; INFO flags bit5 is set for
// VERSION, SAME_HAND and INVALID (the lights show it only after an update:
// updater.c). The half without USB always reports NONE.

typedef enum {
    UPDATE_PRESENCE_NONE      = 0, // no answer from the other half (or this half is not the master)
    UPDATE_PRESENCE_MATCH     = 1, // same fw_version, git hash and updater protocol, other hand
    UPDATE_PRESENCE_VERSION   = 2, // a different fw_version, git hash or updater protocol
    UPDATE_PRESENCE_SAME_HAND = 3, // both halves are built for the same side
    UPDATE_PRESENCE_INVALID   = 4, // the answer fails magic, version or CRC: firmware without presence
} update_presence_status_t;
