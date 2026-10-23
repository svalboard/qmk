// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the updater's foundation: manifest bounds, structure checks,
// boot2 CRC vectors, SHA-512 vectors (vendored from Monocypher), .svup files
// made by make_update.py, and staging-slot flash access against a mock die.
// Run by util/updater_test/run.sh under ASan and UBSan.
//
//   test_updater [CROSS_DIR]   CROSS_DIR: .svup files written by test_make_update.py

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update_manifest.h"
#include "update_image.h"
#include "update_build_info.h"
#include "sha512.h"
#include "boot2_page0.h"

// update_flash.c is built into this file so the tests can reset its state and
// reach its RAM routine directly.
#include "update_flash.c"
#include "update_pair.h"

static int checks, failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                        \
    do {                                                                                                                      \
        long long _a = (long long)(a), _b = (long long)(b);                                                                   \
        checks++;                                                                                                             \
        if (_a != _b) {                                                                                                       \
            failures++;                                                                                                       \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        }                                                                                                                     \
    } while (0)

static void hex(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) != 2 * n) {
        fprintf(stderr, "bad hex length for %zu bytes: %s\n", n, s);
        exit(2);
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

// ---- boot2 CRC (CRC-32/MPEG-2) -------------------------------------------------------

static void test_boot2_crc(void) {
    CHECK_EQ(update_crc32_mpeg2((const uint8_t *)"123456789", 9), 0x0376E6E7u); // the catalog check value

    // Page 0 of a real build (svalboard/trackball/pmw3389/right:sval at 2ee7df613f).
    CHECK_EQ(update_crc32_mpeg2(boot2_page0, 252), 0xd58f0b07u);
    CHECK(update_boot2_valid(boot2_page0));

    uint8_t page[256];
    memset(page, 0x00, sizeof(page));
    CHECK_EQ(update_crc32_mpeg2(page, 252), 0x7065399au);
    CHECK(!update_boot2_valid(page)); // a zeroed page 0 never boots (commit step 2)
    memset(page, 0xFF, sizeof(page));
    CHECK_EQ(update_crc32_mpeg2(page, 252), 0x0b8fd31au);
    CHECK(!update_boot2_valid(page)); // nor does an erased one

    // Any single flipped bit in the code or the stored CRC invalidates it.
    for (int byte = 0; byte < 256; byte += 1) {
        memcpy(page, boot2_page0, sizeof(page));
        page[byte] ^= (uint8_t)(1u << (byte % 8));
        CHECK(!update_boot2_valid(page));
    }
}

// ---- manifest bounds -------------------------------------------------------------------

static const update_device_t test_dev = {
    .hand           = UPDATE_HAND_RIGHT,
    .pointing_id    = UPDATE_POINTING_PMW3389,
    .security_epoch = 3,
    .storage_format = SVAL_UPDATE_STORAGE_FORMAT,
    .release_build  = false,
    .max_image      = SVAL_UPDATE_MAX_IMAGE,
};

static sval_update_manifest_t good_manifest(void) {
    sval_update_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.magic          = UPDATE_MANIFEST_MAGIC;
    m.manifest_ver   = UPDATE_MANIFEST_VERSION;
    m.hand           = UPDATE_HAND_RIGHT;
    m.pointing_id    = UPDATE_POINTING_PMW3389;
    m.keymap_id      = UPDATE_KEYMAP_SVAL;
    m.security_epoch = 3;
    m.storage_format = SVAL_UPDATE_STORAGE_FORMAT;
    m.updater_proto  = UPDATE_PROTOCOL_VERSION;
    m.image_len      = 0x18000;
    m.fw_version     = 1;
    memcpy(m.version, "test", 4);
    return m;
}

static void test_manifest_layout(void) {
    CHECK_EQ(sizeof(sval_update_manifest_t), 108);
    CHECK_EQ(UPDATE_MANIFEST_BYTES, 108); // the MANIFEST op's buffer: no signature after it (protocol 2)
    CHECK_EQ(UPDATE_MANIFEST_VERSION, 2);
    CHECK_EQ(UPDATE_PROTOCOL_VERSION, 4); // 3 plus one approval for both halves and SETTINGS_RESET (D41, D42)
    CHECK_EQ(UPDATE_BAD_SIG, 8);          // retired, never returned; the codes after it keep their values
    CHECK_EQ(UPDATE_BAD_HASH, 9);
    CHECK_EQ(offsetof(sval_update_manifest_t, reserved0), 6); // key_id in version 1
    CHECK_EQ(offsetof(sval_update_manifest_t, flags), 10);
    CHECK_EQ(offsetof(sval_update_manifest_t, security_epoch), 12);
    CHECK_EQ(offsetof(sval_update_manifest_t, fw_version), 20);
    CHECK_EQ(offsetof(sval_update_manifest_t, reserved), 24);
    CHECK_EQ(offsetof(sval_update_manifest_t, version), 28);
    const uint8_t          magic[4] = {'S', 'V', 'U', 'P'};
    sval_update_manifest_t m        = good_manifest();
    CHECK(memcmp(&m.magic, magic, 4) == 0);
}

static void test_manifest_check(void) {
    update_device_t rel = test_dev;
    rel.release_build   = true;
    sval_update_manifest_t m;

#define EXPECT(dev, mutate, status)                          \
    do {                                                     \
        m = good_manifest();                                 \
        mutate;                                              \
        CHECK_EQ(update_manifest_check(&m, &(dev)), status); \
    } while (0)

    EXPECT(test_dev, (void)0, UPDATE_OK);
    EXPECT(test_dev, m.magic ^= 1, UPDATE_INVALID);
    EXPECT(test_dev, m.manifest_ver = 1, UPDATE_UNSUPPORTED); // version 1 (signed) is refused
    EXPECT(test_dev, m.manifest_ver = 3, UPDATE_UNSUPPORTED);
    EXPECT(test_dev, m.manifest_ver = 0, UPDATE_UNSUPPORTED);
    EXPECT(test_dev, m.reserved = 1, UPDATE_INVALID);
    EXPECT(test_dev, m.flags = 0x0004, UPDATE_INVALID);
    EXPECT(test_dev, m.flags = 0x8000, UPDATE_INVALID);
    // reserved0 (version 1's key_id) must be 0
    EXPECT(test_dev, m.reserved0 = 1, UPDATE_INVALID);
    EXPECT(test_dev, m.reserved0 = 2, UPDATE_INVALID);
    EXPECT(test_dev, m.reserved0 = 255, UPDATE_INVALID);
    EXPECT(rel, m.reserved0 = 1, UPDATE_INVALID);
    // a version-1 manifest as v1 tools wrote it (key_id 0 or 1) is refused as UNSUPPORTED, never BAD_SIG
    EXPECT(test_dev, (m.manifest_ver = 1, m.reserved0 = 1), UPDATE_UNSUPPORTED);
    // flags
    EXPECT(test_dev, m.flags = UPDATE_FLAG_RELEASE, UPDATE_OK);
    EXPECT(test_dev, m.flags = UPDATE_FLAG_DIAGNOSTIC, UPDATE_OK); // fine on a non-release build
    EXPECT(rel, (void)0, UPDATE_OK);
    EXPECT(rel, m.flags = UPDATE_FLAG_RELEASE, UPDATE_OK);
    EXPECT(rel, m.flags = UPDATE_FLAG_DIAGNOSTIC, UPDATE_BAD_IMAGE); // release build: no DIAGNOSTIC image
    // gentle (D33): an image for the other hand or another pointing device is taken,
    // on a plain and a release build; the host tools warn and ask instead
    EXPECT(test_dev, m.hand = UPDATE_HAND_LEFT, UPDATE_OK);
    EXPECT(test_dev, m.hand = 7, UPDATE_OK);
    EXPECT(rel, m.hand = UPDATE_HAND_LEFT, UPDATE_OK);
    EXPECT(test_dev, m.pointing_id = UPDATE_POINTING_PMW3360, UPDATE_OK);
    EXPECT(test_dev, m.pointing_id = UPDATE_POINTING_NONE, UPDATE_OK);
    EXPECT(test_dev, m.pointing_id = UPDATE_POINTING_AUTO, UPDATE_OK); // auto image on a single-module build
    EXPECT(rel, (m.hand = UPDATE_HAND_LEFT, m.pointing_id = UPDATE_POINTING_TRACKPOINT), UPDATE_OK);
    {
        update_device_t auto_dev = test_dev; // a single-module image on an auto build
        auto_dev.pointing_id     = UPDATE_POINTING_AUTO;
        EXPECT(auto_dev, (void)0, UPDATE_OK);
        EXPECT(auto_dev, m.pointing_id = UPDATE_POINTING_AZOTEQ, UPDATE_OK);
    }
    // a mismatched hand never hides a real refusal
    EXPECT(test_dev, (m.hand = UPDATE_HAND_LEFT, m.security_epoch = 2), UPDATE_EPOCH);
    EXPECT(rel, (m.pointing_id = UPDATE_POINTING_PMW3360, m.flags = UPDATE_FLAG_DIAGNOSTIC), UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.keymap_id = 99, UPDATE_OK); // display only
    EXPECT(test_dev, m.fw_version = 0, UPDATE_OK); // downgrades are allowed (D19)
    // floors
    EXPECT(test_dev, m.security_epoch = 2, UPDATE_EPOCH);
    EXPECT(test_dev, m.security_epoch = 4, UPDATE_OK);
    EXPECT(test_dev, m.storage_format = SVAL_UPDATE_STORAGE_FORMAT - 1, UPDATE_STORAGE);
    EXPECT(test_dev, m.storage_format = SVAL_UPDATE_STORAGE_FORMAT + 1, UPDATE_OK);
    // length bounds
    EXPECT(test_dev, m.image_len = 0, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = 0x100, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = 0x1FF, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = 0x200, UPDATE_OK);
    EXPECT(test_dev, m.image_len = 0x201, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = 0x2FF, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = SVAL_UPDATE_MAX_IMAGE, UPDATE_OK);
    EXPECT(test_dev, m.image_len = SVAL_UPDATE_MAX_IMAGE - 1, UPDATE_BAD_IMAGE);
    EXPECT(test_dev, m.image_len = SVAL_UPDATE_MAX_IMAGE + 0x100, UPDATE_TOO_LARGE);
    EXPECT(test_dev, m.image_len = 0x80000000u, UPDATE_TOO_LARGE);
    EXPECT(test_dev, m.image_len = 0xFFFFFF00u, UPDATE_TOO_LARGE);
    EXPECT(test_dev, m.image_len = 0xFFFFFFFFu, UPDATE_TOO_LARGE);
#undef EXPECT
}

// ---- image head checks ---------------------------------------------------------------

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void make_head(uint8_t head[UPDATE_HEAD_BYTES], uint32_t sp, uint32_t reset) {
    memcpy(head, boot2_page0, 256);
    put32(head + 0x100, sp);
    put32(head + 0x104, reset);
}

static void test_head_check(void) {
    uint8_t        head[UPDATE_HEAD_BYTES];
    const uint32_t len = 0x18000;

    make_head(head, 0x20041000, 0x100001F7);
    CHECK_EQ(update_image_head_check(head, len), UPDATE_OK);
    // length rules apply here too
    CHECK_EQ(update_image_head_check(head, 0x1F00), UPDATE_OK);
    CHECK_EQ(update_image_head_check(head, 0x180), UPDATE_BAD_IMAGE);
    CHECK_EQ(update_image_head_check(head, len + 1), UPDATE_BAD_IMAGE);
    CHECK_EQ(update_image_head_check(head, 0xFFFFFF00u), UPDATE_BAD_IMAGE);
    // boot2
    head[17] ^= 0x40;
    CHECK_EQ(update_image_head_check(head, len), UPDATE_BAD_IMAGE);
    make_head(head, 0x20041000, 0x100001F7);
    head[253] ^= 1;
    CHECK_EQ(update_image_head_check(head, len), UPDATE_BAD_IMAGE);
    // initial SP within [0x20000000, 0x20042000]
    const struct {
        uint32_t        sp;
        update_status_t want;
    } sps[] = {
        {0x20000000, UPDATE_OK}, {0x20042000, UPDATE_OK}, {0x1FFFFFFC, UPDATE_BAD_IMAGE}, {0x20042004, UPDATE_BAD_IMAGE}, {0x00000000, UPDATE_BAD_IMAGE}, {0xFFFFFFFF, UPDATE_BAD_IMAGE}, {0x10000400, UPDATE_BAD_IMAGE},
    };
    for (size_t i = 0; i < sizeof(sps) / sizeof(sps[0]); i++) {
        make_head(head, sps[i].sp, 0x100001F7);
        CHECK_EQ(update_image_head_check(head, len), sps[i].want);
    }
    // reset vector: odd, inside [0x10000100, 0x10000000 + len)
    const struct {
        uint32_t        reset;
        update_status_t want;
    } resets[] = {
        {0x10000101, UPDATE_OK}, {0x10000000 + len - 1, UPDATE_OK}, {0x100001F6, UPDATE_BAD_IMAGE}, {0x100000FF, UPDATE_BAD_IMAGE}, {0x10000001, UPDATE_BAD_IMAGE}, {0x10000000 + len + 1, UPDATE_BAD_IMAGE}, {0x20000001, UPDATE_BAD_IMAGE}, {0x00000001, UPDATE_BAD_IMAGE}, {0xFFFFFFFF, UPDATE_BAD_IMAGE},
    };
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        make_head(head, 0x20041000, resets[i].reset);
        CHECK_EQ(update_image_head_check(head, len), resets[i].want);
    }
    // a zeroed or erased page 0 always fails
    memset(head, 0, sizeof(head));
    CHECK_EQ(update_image_head_check(head, len), UPDATE_BAD_IMAGE);
    memset(head, 0xFF, sizeof(head));
    CHECK_EQ(update_image_head_check(head, len), UPDATE_BAD_IMAGE);
}

// ---- SHA-512 (vendor/sha512.c, from Monocypher) --------------------------------------------

static void test_sha512(void) {
    uint8_t got[64], want[64];
    crypto_sha512(got, (const uint8_t *)"abc", 3);
    hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", want, 64);
    CHECK(memcmp(got, want, 64) == 0);
    crypto_sha512(got, (const uint8_t *)"", 0);
    hex("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", want, 64);
    CHECK(memcmp(got, want, 64) == 0);
    // Incremental, in uneven pieces, as the verify slices will feed it.
    static uint8_t big[3000];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 7 + 1);
    uint8_t           one[64];
    crypto_sha512_ctx ctx;
    crypto_sha512(one, big, sizeof(big));
    crypto_sha512_init(&ctx);
    for (size_t off = 0, step = 1; off < sizeof(big); off += step, step = step * 3 % 517 + 1) {
        crypto_sha512_update(&ctx, big + off, off + step > sizeof(big) ? sizeof(big) - off : step);
    }
    crypto_sha512_final(&ctx, got);
    CHECK(memcmp(got, one, 64) == 0);
}

static void test_build_info(void) {
    CHECK(memcmp(sval_update_build_info.magic, "SVBI", 4) == 0);
    CHECK_EQ(sval_update_build_info.info_ver, 3);
    CHECK_EQ(sval_update_build_info.updater_proto, UPDATE_PROTOCOL_VERSION);
    CHECK_EQ(sval_update_build_info.flags & (0x02 | 0x20), 0); // retired key flags stay clear
    CHECK_EQ(sval_update_build_info.reserved0, 0);
    CHECK_EQ(sval_update_build_info.flags & UPDATE_BUILD_RELEASE, 0); // not a release build
}

// ---- make_update.py outputs ------------------------------------------------------------

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void test_cross(const char *dir) {
    char   path[4096];
    size_t len;
    snprintf(path, sizeof(path), "%s/index.txt", dir);
    FILE *idx = fopen(path, "r");
    CHECK(idx != NULL);
    if (!idx) return;
    char name[256], expect[32];
    int  files = 0;
    while (fscanf(idx, "%255s %31s", name, expect) == 2) {
        files++;
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        uint8_t *buf = read_file(path, &len);
        CHECK(buf != NULL && len >= UPDATE_MANIFEST_BYTES);
        if (!buf || len < UPDATE_MANIFEST_BYTES) continue;
        // A .svup is the manifest, then the image: nothing between them.
        const sval_update_manifest_t *m      = (const sval_update_manifest_t *)buf;
        const uint8_t                *image  = buf + UPDATE_MANIFEST_BYTES;
        size_t                        ilen   = len - UPDATE_MANIFEST_BYTES;
        bool                          ok     = strcmp(expect, "ok") == 0;
        bool                          badimg = strcmp(expect, "badimage") == 0;
        update_device_t               dev    = test_dev;
        dev.hand                             = m->hand;
        dev.pointing_id                      = m->pointing_id;
        dev.security_epoch                   = 0;
        uint8_t h[64];
        crypto_sha512(h, image, ilen);
        if (ok || badimg) {
            CHECK_EQ(m->image_len, ilen);
            CHECK(memcmp(h, m->sha512, 64) == 0);
            CHECK_EQ(update_manifest_check(m, &dev), UPDATE_OK);
            if (ilen >= UPDATE_HEAD_BYTES) CHECK_EQ(update_image_head_check(image, m->image_len), ok ? UPDATE_OK : UPDATE_BAD_IMAGE);
        } else if (strcmp(expect, "badhash") == 0) {
            CHECK_EQ(update_manifest_check(m, &dev), UPDATE_OK);
            CHECK(memcmp(h, m->sha512, 64) != 0);
        } else if (strcmp(expect, "v1") == 0) {
            CHECK_EQ(update_manifest_check(m, &dev), UPDATE_UNSUPPORTED);
        } else {
            CHECK(!"unknown expectation in index.txt");
        }
        if (failures) fprintf(stderr, "  (in %s, expected %s)\n", name, expect);
        free(buf);
    }
    fclose(idx);
    CHECK(files >= 4);
    printf("cross-checked %d make_update.py outputs\n", files);
}

// ---- staging-slot flash (mock die) --------------------------------------------------------

uint8_t        update_host_flash[UPDATE_HOST_FLASH_BYTES] __attribute__((aligned(4096)));
static uint8_t before[UPDATE_HOST_FLASH_BYTES];
const uint8_t  BOOT2_ROM[256] = BOOT2_PAGE0_BYTES;
static bool    xip_on         = true, irq_off, connected;
static int     erase_ops, program_ops, sequence_errors, outside_ops;
static int     tear_next; // the next ROM erase/program leaves random bytes
static int     sector_erases, block_erases;
// Simulated time (test_session.c's clock). Each ROM flash operation costs what
// it would on a W25Q128JV-class die: a 4 KiB sector erase 45 ms and a 64 KiB
// block erase 150 ms (typical), a page program 3 ms (maximum).
#define SIM_SECTOR_ERASE_US 45000u
#define SIM_BLOCK_ERASE_US 150000u
#define SIM_PAGE_PROGRAM_US 3000u
static uint64_t sim_us;
static uint8_t  capacity = 0x18;
static uint32_t rng      = 12345;

// Where ROM erases and programs may land: the slot while staging (invariant
// (a)); test_commit.c widens it to [0, round_up(image_len, 64 KiB)) for the
// commit (invariant (b)). Anything else counts in outside_ops and is not done.
static uint32_t allow_lo = SVAL_UPDATE_BASE, allow_hi = SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE;
static bool     multi_page; // the commit programs up to a sector per ROM call; staging one page

// Power cuts: after ROM op number cut_at (1-based, counted in flash_ops) has
// run, or been torn when cut_tear is set, control jumps to cut_env.
static jmp_buf cut_env;
static int     flash_ops, cut_at;
static bool    cut_tear;

// A program that does not take: the byte at bad_at keeps its old value on the
// bad_nth ROM program covering it, and on the bad_times - 1 after that.
static uint32_t bad_at = 0xFFFFFFFFu;
static int      bad_nth, bad_times, bad_seen;

// Watchdog accounting for the commit: worst-case flash time since the last
// feed (W25Q128JV maxima: sector erase 400 ms, block erase 2 s, page program
// 3 ms), while the mock watchdog is enabled.
#define WORST_SECTOR_ERASE_US 400000u
#define WORST_BLOCK_ERASE_US 2000000u
#define WORST_PAGE_PROGRAM_US 3000u
static bool     mock_wd_on;
static uint64_t wd_since_feed_us, wd_gap_max_us;

// Every ROM erase and program, in order.
typedef struct {
    char     op; // 'E' or 'P'
    uint32_t addr, count;
    bool     zeros; // a program of all zero bytes
} oplog_t;
static oplog_t oplog[1024];
static int     noplog;

static void wd_spend(uint64_t us) {
    if (!mock_wd_on) return;
    wd_since_feed_us += us;
    if (wd_since_feed_us > wd_gap_max_us) wd_gap_max_us = wd_since_feed_us;
}

static void log_op(char op, uint32_t addr, uint32_t count, const uint8_t *data) {
    bool zeros = data != NULL;
    for (uint32_t i = 0; data && i < count; i++)
        zeros &= data[i] == 0;
    if (noplog < (int)(sizeof(oplog) / sizeof(oplog[0]))) oplog[noplog] = (oplog_t){op, addr, count, zeros};
    noplog++;
}

// Called after each counted ROM op (test_commit.c: a slot byte going bad
// mid-commit).
static void (*after_op)(int op);

// Counts a ROM op that changes the die: true when the cut comes after it.
static bool cut_now(void) {
    flash_ops++;
    if (after_op) after_op(flash_ops);
    return cut_at && flash_ops == cut_at;
}

static uint8_t rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return (uint8_t)(rng >> 16);
}

static void mock_connect(void) {
    if (!irq_off || !xip_on) sequence_errors++;
    connected = true;
}
static void mock_exit_xip(void) {
    if (!connected) sequence_errors++;
    xip_on = false;
}
static void mock_flush(void) {
    if (xip_on) sequence_errors++;
}
// The boot relay of a pair UF2 (test_pair_uf2.c) may also erase the record
// sector and program its state page, and nothing else; off everywhere else,
// so any session that touched that sector counts in outside_ops.
static bool allow_pair_sector;
static int  pair_sector_ops;
static bool slot_only(uint32_t addr, size_t count) {
    // Invariant (a): during staging, only [SLOT_BASE, SLOT_BASE + SIZE) changes;
    // (b) during the commit, only [0, round_up(image_len, 64 KiB)).
    bool pair = addr < UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_SECTOR_BYTES && addr + count > UPDATE_PAIR_RECORD_OFFSET;
    if (pair) pair_sector_ops++;
    if (pair && allow_pair_sector) {
        bool ok = (addr == UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_STATE_OFFSET && count == 256) || (addr == UPDATE_PAIR_RECORD_OFFSET && count == UPDATE_PAIR_SECTOR_BYTES);
        if (!ok) outside_ops++;
        return ok;
    }
    bool inside = addr >= allow_lo && addr <= allow_hi && count <= allow_hi - addr;
    if (!inside) outside_ops++;
    return inside;
}
// A ROM erase or program out of sequence (XIP still on, interrupts on) or with
// bad arguments counts in sequence_errors and changes nothing on the die: on
// hardware it would hang or crash, so it must never look like it worked.
static void mock_erase(uint32_t addr, size_t count, uint32_t block_size, uint8_t cmd) {
    erase_ops++;
    // one aligned 64 KiB block or one aligned 4 KiB sector, passed as the SDK does
    bool block = count == 0x10000 && addr % 0x10000 == 0, sector = count == 0x1000 && addr % 0x1000 == 0;
    if (xip_on || !irq_off || !(block || sector) || block_size != 0x10000 || cmd != 0xD8) {
        sequence_errors++;
        log_op('e', addr, (uint32_t)count, NULL);
        return;
    }
    if (block) block_erases++;
    if (sector) sector_erases++;
    sim_us += block ? SIM_BLOCK_ERASE_US : SIM_SECTOR_ERASE_US;
    wd_spend(block ? WORST_BLOCK_ERASE_US : WORST_SECTOR_ERASE_US);
    log_op('E', addr, (uint32_t)count, NULL);
    if (!slot_only(addr, count)) return;
    bool cut = cut_now();
    if (cut && cut_tear) tear_next = 1;
    for (size_t i = 0; i < count; i++)
        update_host_flash[addr + i] = tear_next ? rnd() : 0xFF;
    tear_next = 0;
    if (cut) longjmp(cut_env, 1);
}
static void mock_program(uint32_t addr, const uint8_t *data, size_t count) {
    program_ops++;
    if (xip_on || !irq_off || addr % 256 || count == 0 || count % 256 || count > (multi_page ? 4096u : 256u)) {
        sequence_errors++;
        log_op('p', addr, (uint32_t)count, NULL);
        return;
    }
    sim_us += SIM_PAGE_PROGRAM_US * (count / 256);
    wd_spend(WORST_PAGE_PROGRAM_US * (count / 256));
    log_op('P', addr, (uint32_t)count, data);
    if (!slot_only(addr, count)) return;
    bool    cut  = cut_now();
    bool    bad  = bad_at >= addr && bad_at < addr + count && ++bad_seen >= bad_nth && bad_times > 0;
    uint8_t keep = bad ? update_host_flash[bad_at] : 0;
    if (cut && cut_tear) tear_next = 1;
    for (size_t i = 0; i < count; i++)
        update_host_flash[addr + i] = tear_next ? rnd() : (update_host_flash[addr + i] & data[i]); // NOR clears bits
    if (bad) {
        update_host_flash[bad_at] = keep; // this byte did not take
        bad_times--;
    }
    tear_next = 0;
    if (cut) longjmp(cut_env, 1);
}

void *rom_func_lookup_inline(uint32_t code) {
    switch (code) {
        case ROM_FUNC_CONNECT_INTERNAL_FLASH:
            return (void *)mock_connect;
        case ROM_FUNC_FLASH_EXIT_XIP:
            return (void *)mock_exit_xip;
        case ROM_FUNC_FLASH_RANGE_ERASE:
            return (void *)mock_erase;
        case ROM_FUNC_FLASH_RANGE_PROGRAM:
            return (void *)mock_program;
        case ROM_FUNC_FLASH_FLUSH_CACHE:
            return (void *)mock_flush;
        default:
            return NULL;
    }
}
uint32_t save_and_disable_interrupts(void) {
    bool was = irq_off;
    irq_off  = true;
    return was;
}
void restore_interrupts(uint32_t status) {
    irq_off = status;
}
void flash_do_cmd(const uint8_t *tx, uint8_t *rx, size_t count) {
    if (!irq_off || count != 4 || tx[0] != 0x9F) sequence_errors++;
    rx[0] = 0xFF;
    rx[1] = 0xEF; // Winbond
    rx[2] = 0x40;
    rx[3] = capacity;
}
void update_host_enter_xip(const uint32_t *copy) {
    if (memcmp(copy, BOOT2_ROM, 256) != 0) sequence_errors++; // XIP comes back only through a good boot2 copy
    connected = false;
    xip_on    = true;
}

static void die_reset(void) {
    for (size_t i = 0; i < sizeof(update_host_flash); i++)
        update_host_flash[i] = rnd() | 0x01; // never all 0xFF, rarely erased-looking
    memset(update_host_flash + SVAL_UPDATE_BASE, 0x00, SVAL_UPDATE_SIZE);
    probed    = false; // update_flash.c state
    erase_ops = program_ops = sequence_errors = outside_ops = tear_next = sector_erases = block_erases = 0;
    capacity                                                                                           = 0x18;
    allow_lo                                                                                           = SVAL_UPDATE_BASE;
    allow_hi                                                                                           = SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE;
    multi_page                                                                                         = false;
    allow_pair_sector                                                                                  = false;
    pair_sector_ops                                                                                    = 0;
    flash_ops = cut_at = noplog = 0;
    cut_tear                    = false;
    bad_at                      = 0xFFFFFFFFu;
    bad_nth = bad_times = bad_seen = 0;
    after_op                       = NULL;
    mock_wd_on                     = false;
    wd_since_feed_us = wd_gap_max_us = 0;
    xip_on                           = true;
    irq_off = connected = false;
}

static void snap(void) {
    memcpy(before, update_host_flash, sizeof(before));
}

// Nothing outside [lo, hi) (flash offsets) changed since snap().
static bool unchanged_outside(uint32_t lo, uint32_t hi) {
    return memcmp(before, update_host_flash, lo) == 0 && memcmp(before + hi, update_host_flash + hi, sizeof(before) - hi) == 0;
}

static void test_session(void);

static void test_flash(void) {
    const uint32_t slot_lo = SVAL_UPDATE_BASE, slot_hi = SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE;
    uint8_t        page[256];
    for (int i = 0; i < 256; i++)
        page[i] = (uint8_t)(i ^ 0x5A);

    // JEDEC and availability
    die_reset();
    CHECK(update_flash_available());
    uint8_t id[3];
    update_flash_jedec(id);
    CHECK(id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18);

    // a die that is not 16 MiB: nothing is touched (D26)
    die_reset();
    capacity = 0x15; // 2 MiB
    snap();
    CHECK(!update_flash_available());
    CHECK_EQ(update_flash_erase_block(0), UPDATE_UNAVAILABLE);
    CHECK_EQ(update_flash_program_page(0, page), UPDATE_UNAVAILABLE);
    CHECK(erase_ops == 0 && program_ops == 0);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);

    // erase every block of the slot, one at a time: only that block changes
    die_reset();
    for (uint32_t off = 0; off < SVAL_UPDATE_SIZE; off += UPDATE_FLASH_BLOCK) {
        snap();
        CHECK_EQ(update_flash_erase_block(off), UPDATE_OK);
        CHECK(unchanged_outside(slot_lo + off, slot_lo + off + UPDATE_FLASH_BLOCK));
        CHECK(update_flash_block_erased(off));
    }
    CHECK_EQ(erase_ops, SVAL_UPDATE_SIZE / UPDATE_FLASH_BLOCK);
    // an erased block is not erased again
    CHECK_EQ(update_flash_erase_block(0), UPDATE_OK);
    CHECK_EQ(erase_ops, SVAL_UPDATE_SIZE / UPDATE_FLASH_BLOCK);
    CHECK(xip_on && !irq_off);

    // erase refusals: misaligned, outside, wrapping; nothing touched
    snap();
    const uint32_t bad_erase[] = {0x1000, 0xFF, SVAL_UPDATE_SIZE, SVAL_UPDATE_SIZE + UPDATE_FLASH_BLOCK, 0xFFFF0000u, 0x80000000u, 0xFFFFFFFFu, (uint32_t)-SVAL_UPDATE_BASE};
    for (size_t i = 0; i < sizeof(bad_erase) / sizeof(bad_erase[0]); i++)
        CHECK_EQ(update_flash_erase_block(bad_erase[i]), UPDATE_INVALID);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);

    // program: first and last page; then refusals
    snap();
    CHECK_EQ(update_flash_program_page(0, page), UPDATE_OK);
    CHECK_EQ(update_flash_program_page(SVAL_UPDATE_SIZE - 256, page), UPDATE_OK);
    CHECK(memcmp(update_host_flash + slot_lo, page, 256) == 0);
    CHECK(memcmp(update_host_flash + slot_hi - 256, page, 256) == 0);
    CHECK(unchanged_outside(slot_lo, slot_hi));
    int ops = program_ops;
    snap();
    CHECK_EQ(update_flash_program_page(0, page), UPDATE_INVALID); // not erased
    CHECK_EQ(update_flash_program_page(0x80, page), UPDATE_INVALID);
    CHECK_EQ(update_flash_program_page(SVAL_UPDATE_SIZE, page), UPDATE_INVALID);
    CHECK_EQ(update_flash_program_page(0xFFFFFF00u, page), UPDATE_INVALID);
    CHECK_EQ(update_flash_program_page((uint32_t)-SVAL_UPDATE_BASE, page), UPDATE_INVALID);
    CHECK_EQ(update_flash_program_page(256, NULL), UPDATE_INVALID);
    CHECK_EQ(program_ops, ops);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);

    // source data in "flash": copied to RAM before XIP goes off
    CHECK_EQ(update_flash_program_page(256, update_host_flash + 0x1000), UPDATE_OK);
    CHECK(memcmp(update_host_flash + slot_lo + 256, update_host_flash + 0x1000, 256) == 0);

    // torn operations read back wrong and are reported, and stay in the slot
    snap();
    tear_next = 1;
    CHECK_EQ(update_flash_program_page(512, page), UPDATE_FLASH_ERR);
    CHECK(unchanged_outside(slot_lo + 512, slot_lo + 768));
    snap();
    tear_next = 1;
    CHECK_EQ(update_flash_erase_block(0), UPDATE_FLASH_ERR);
    CHECK(unchanged_outside(slot_lo, slot_lo + UPDATE_FLASH_BLOCK));
    CHECK_EQ(update_flash_erase_block(0), UPDATE_OK); // and a retry clears it

    // the RAM routine's own guard: called directly with addresses outside the
    // slot, it makes no ROM call at all
    snap();
    ops                      = erase_ops + program_ops;
    const uint32_t bad_abs[] = {0, SVAL_UPDATE_BASE - UPDATE_FLASH_BLOCK, slot_hi, slot_hi - 0x100, IDENTITY_SECTOR_A, WEAR_LEVELING_RP2040_FLASH_BASE, 0xFFFFFF00u};
    for (size_t i = 0; i < sizeof(bad_abs) / sizeof(bad_abs[0]); i++) {
        irq_off = true;
        slot_flash_op(bad_abs[i], NULL, UPDATE_FLASH_BLOCK);
        slot_flash_op(bad_abs[i], NULL, UPDATE_FLASH_SECTOR);
        if (bad_abs[i] != slot_hi - 0x100) {
            slot_flash_op(bad_abs[i], page, 0);
        }
        irq_off = false;
    }
    // inside the slot, but an erase size other than a sector or a block, or
    // misaligned for its size: no ROM call either
    irq_off = true;
    slot_flash_op(slot_lo, NULL, 0x2000);
    slot_flash_op(slot_lo, NULL, 0);
    slot_flash_op(slot_lo + 0x1000, NULL, UPDATE_FLASH_BLOCK);
    slot_flash_op(slot_lo + 0x100, NULL, UPDATE_FLASH_SECTOR);
    slot_flash_op(slot_lo + 0x80, page, 0);
    irq_off = false;
    CHECK_EQ(erase_ops + program_ops, ops);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);

    // sector erase: every sector of the slot, one at a time; only that sector changes
    die_reset();
    for (uint32_t off = 0; off < SVAL_UPDATE_SIZE; off += UPDATE_FLASH_SECTOR) {
        if (off % 0x40000 == 0 || off + UPDATE_FLASH_SECTOR == SVAL_UPDATE_SIZE) snap(); // a whole-die compare is slow
        CHECK_EQ(update_flash_erase_sector(off), UPDATE_OK);
        CHECK(update_flash_sector_erased(off));
        if (off % 0x40000 == 0 || off + UPDATE_FLASH_SECTOR == SVAL_UPDATE_SIZE) CHECK(unchanged_outside(slot_lo + off, slot_lo + off + UPDATE_FLASH_SECTOR));
    }
    CHECK_EQ(sector_erases, SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    CHECK_EQ(block_erases, 0);
    CHECK_EQ(update_flash_erase_sector(0), UPDATE_OK); // already erased: no ROM call
    CHECK_EQ(sector_erases, SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    snap();
    const uint32_t bad_sector[] = {0x80, 0x100, 0xFFF, SVAL_UPDATE_SIZE, SVAL_UPDATE_SIZE - 0x800, 0xFFFFF000u, (uint32_t)-SVAL_UPDATE_BASE};
    for (size_t i = 0; i < sizeof(bad_sector) / sizeof(bad_sector[0]); i++)
        CHECK_EQ(update_flash_erase_sector(bad_sector[i]), UPDATE_INVALID);
    CHECK(!update_flash_sector_erased(0x80));
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
    memset(update_host_flash + slot_lo + 0x3000 + 77, 0x12, 1);
    snap();
    tear_next = 1;
    CHECK_EQ(update_flash_erase_sector(0x3000), UPDATE_FLASH_ERR);
    CHECK(unchanged_outside(slot_lo + 0x3000, slot_lo + 0x4000));
    CHECK_EQ(update_flash_erase_sector(0x3000), UPDATE_OK);
    capacity = 0x17;
    probed   = false;
    CHECK_EQ(update_flash_erase_sector(0x3000), UPDATE_UNAVAILABLE);
    capacity = 0x18;
    probed   = false;

    // the slot view
    CHECK(update_slot_read(0, SVAL_UPDATE_SIZE) == update_host_flash + slot_lo);
    CHECK(update_slot_read(SVAL_UPDATE_SIZE, 0) != NULL);
    CHECK(update_slot_read(SVAL_UPDATE_SIZE, 1) == NULL);
    CHECK(update_slot_read(1, SVAL_UPDATE_SIZE) == NULL);
    CHECK(update_slot_read(0xFFFFFFFFu, 2) == NULL);
    CHECK(!update_slot_range_ok(2, 0xFFFFFFFFu));

    CHECK_EQ(sequence_errors, 0);
    CHECK_EQ(outside_ops, 0);
}

// The session state machine: updater.c and update_gesture.c against these mocks.
#include "test_session.c"

#ifdef SVAL_TEST_REAL_COMMIT
// The commit routine itself (update_commit.c) against the same mocks.
#    include "test_commit.c"
#endif

// M2: the relay to the other half and its session (update_split.c).
#include "test_relay.c"

// Protocol 4 (D41-D43): one approval for both halves, SETTINGS_RESET.
#include "test_pair.c"

// The pair UF2's boot relay (update_pair.c, docs/pair-uf2.md).
#include "test_pair_uf2.c"

#ifdef SVAL_UPDATER_HOST_LIB
// Entry points for tests/sval_updater/test_sval_update_tool.py (ctypes).
void host_lib_reset(int commit_available) {
    fresh(0x3000);
    commit_avail = commit_available != 0;
}
void host_via(uint8_t *pkt, uint32_t client) {
    port_client = client;
    updater_via_command(pkt, RAW_EPSIZE);
    port_client = 0;
}
void host_passes(int n) {
    for (int i = 0; i < n; i++)
        pass();
}
void host_chord(void) {
    chord();
}
int host_state(void) {
    return updater_state();
}
const uint8_t *host_slot(void) {
    return update_host_flash + SVAL_UPDATE_BASE;
}
int host_commit_runs(void) {
    return commit_runs;
}
uint32_t host_commit_crc(void) {
    return commit_crc;
}
// M2: the simulated other half (test_session.c). on: presence pings, a left
// half with a TrackPoint on the same release; both halves "reset into" what
// they commit.
void host_lib_split(int on) {
    presence_sim         = on != 0;
    auto_shadow          = on != 0;
    slave_commit_resets  = on != 0;
    master_commit_resets = on != 0;
    last_ping_us         = 0;
}
int host_slave_commits(void) {
    return slave_commit_runs;
}
uint32_t host_slave_commit_crc(void) {
    return slave_commit_crc;
}
int host_master_reboots(void) {
    return master_reboots;
}
uint32_t host_slave_fw(void) {
    return slave_id.fw_version;
}
uint32_t host_master_fw(void) {
    return master_id.fw_version;
}
int host_slave_state(void) {
    return update_split_slave_state();
}
int host_link_paused(void) {
    return link_paused;
}
int host_chord_done(void) {
    return update_gesture_done(NULL);
}
// SETTINGS_RESET (D42): each half's record invalidations and resets.
int host_master_invalidations(void) {
    return master_invalidations;
}
int host_slave_invalidations(void) {
    return slave_invalidations;
}
int host_master_soft_resets(void) {
    return master_soft_resets;
}
int host_master_reset_mode(void) {
    return master_reset_mode;
}
int host_slave_reset_mode(void) {
    return slave_reset_mode;
}
int host_slave_soft_resets(void) {
    return slave_soft_resets;
}
#else
int main(int argc, char **argv) {
#    ifdef SVAL_UPDATE_NO_CHORD
    // The no-chord dev build (D43): its protocol 4 cases only; the rest of
    // these tests expect the chord.
    (void)argc;
    (void)argv;
    test_pair_no_chord();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
#    endif
#    ifdef SVAL_TEST_REAL_COMMIT
    (void)argc;
    (void)argv;
    test_commit();
    test_relay_real_commit();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
#    endif
    test_boot2_crc();
    test_manifest_layout();
    test_manifest_check();
    test_head_check();
    test_sha512();
    test_build_info();
    test_flash();
    if (argc > 1) test_cross(argv[1]);
    test_session();
    test_relay();
    test_pair();
    test_pair_uf2(argc > 1 ? argv[1] : NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
