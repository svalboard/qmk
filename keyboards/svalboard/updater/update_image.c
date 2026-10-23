// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Structure checks for the updater (see update_image.h).

#include <string.h>
#include "update_image.h"
#include "update_build_info.h"
#include "update_self.h"

// ---- this build ------------------------------------------------------------------

#ifndef SVAL_UPDATER_HOST_TEST
void update_device_self(update_device_t *dev) {
    dev->hand           = UPDATE_RUNNING_HAND;
    dev->pointing_id    = UPDATE_SELF_POINTING;
    dev->security_epoch = SVAL_UPDATE_SECURITY_EPOCH;
    dev->storage_format = SVAL_UPDATE_STORAGE_FORMAT;
#    ifdef SVAL_UPDATE_RELEASE
    dev->release_build = true;
#    else
    dev->release_build = false;
#    endif
    dev->max_image = SVAL_UPDATE_MAX_IMAGE;
}
#endif

// ---- checks ------------------------------------------------------------------------

uint32_t update_crc32_mpeg2_update(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (uint8_t b = 0; b < 8; b++) crc = (crc << 1) ^ (0x04C11DB7u & -(crc >> 31));
    }
    return crc;
}

uint32_t update_crc32_mpeg2(const uint8_t *p, size_t n) {
    return update_crc32_mpeg2_update(UPDATE_CRC32_INIT, p, n);
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool update_boot2_valid(const uint8_t page0[UPDATE_BOOT2_BYTES]) {
    return update_crc32_mpeg2(page0, UPDATE_BOOT2_CRC_OFFSET) == get32(page0 + UPDATE_BOOT2_CRC_OFFSET);
}

update_status_t update_manifest_check(const sval_update_manifest_t *m, const update_device_t *dev) {
    if (m->magic != UPDATE_MANIFEST_MAGIC) return UPDATE_INVALID;
    if (m->manifest_ver != UPDATE_MANIFEST_VERSION) return UPDATE_UNSUPPORTED;
    if (m->reserved0 != 0 || m->reserved != 0 || (m->flags & ~UPDATE_FLAGS_KNOWN)) return UPDATE_INVALID;
    // A release build takes no DIAGNOSTIC image.
    if (dev->release_build && (m->flags & UPDATE_FLAG_DIAGNOSTIC)) return UPDATE_BAD_IMAGE;
    // The manifest's hand and pointing_id are not checked (D33): an image built for
    // the other side or another pointing module is taken, since moving an MCU between
    // hands or modules is a valid use. The request's hand picks the half; the host
    // tools warn about a mismatch and ask first.
    // Downgrade floors (D19).
    if (m->security_epoch < dev->security_epoch) return UPDATE_EPOCH;
    if (m->storage_format < dev->storage_format) return UPDATE_STORAGE;
    // Length.
    if (m->image_len > dev->max_image) return UPDATE_TOO_LARGE;
    if (m->image_len < UPDATE_IMAGE_MIN || m->image_len % UPDATE_IMAGE_ALIGN) return UPDATE_BAD_IMAGE;
    return UPDATE_OK;
}

update_status_t update_image_head_check(const uint8_t head[UPDATE_HEAD_BYTES], uint32_t image_len) {
    // Length rules again (callers pass the manifest's), plus a bound that keeps
    // the reset-vector range below from wrapping.
    if (image_len < UPDATE_IMAGE_MIN || image_len % UPDATE_IMAGE_ALIGN || image_len > UPDATE_FLASH_DIE_MAX) return UPDATE_BAD_IMAGE;
    if (!update_boot2_valid(head)) return UPDATE_BAD_IMAGE;
    uint32_t sp = get32(head + UPDATE_VECTORS_OFFSET), reset = get32(head + UPDATE_VECTORS_OFFSET + 4);
    if (sp < UPDATE_SP_MIN || sp > UPDATE_SP_MAX) return UPDATE_BAD_IMAGE;
    // Thumb: odd, and inside the image after the boot2 page.
    if (!(reset & 1) || reset < UPDATE_XIP_BASE + UPDATE_VECTORS_OFFSET || reset >= UPDATE_XIP_BASE + image_len) return UPDATE_BAD_IMAGE;
    return UPDATE_OK;
}

// ---- the image's build-info records (D45) ---------------------------------------------

void update_build_scan_init(update_build_scan_t *sc) {
    memset(sc, 0, sizeof(*sc));
}

// Byte by byte, through a 6-byte window. The magic is compared a byte at a
// time (win is byte-aligned, so the compiler cannot merge the compares into one
// word against an "SVBI" literal): the scanner itself never adds a record-like
// "SVBI" to the image it is built into.
void update_build_scan(update_build_scan_t *sc, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        for (size_t k = 0; k + 1 < sizeof(sc->win); k++) sc->win[k] = sc->win[k + 1];
        sc->win[sizeof(sc->win) - 1] = p[i];
        if (sc->seen < sizeof(sc->win)) sc->seen++;
        if (sc->seen == sizeof(sc->win) && sc->win[0] == 'S' && sc->win[1] == 'V' && sc->win[2] == 'B' && sc->win[3] == 'I' && sc->win[4] == UPDATE_BUILD_INFO_VERSION) {
            if (sc->records < 0xFF) sc->records++;
            sc->flags |= sc->win[5];
        }
    }
}

update_status_t update_image_build_check(const update_build_scan_t *sc, const update_device_t *dev) {
    if (dev->release_build && sc->records && (sc->flags & (UPDATE_BUILD_NO_CHORD | UPDATE_BUILD_DEV_KEEP))) return UPDATE_BAD_IMAGE;
    return UPDATE_OK;
}
