// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host test for the side record of the either-hand builds (keyboards/svalboard/side.c):
// the boot-time priority (strap, record, migration, unknown), the record
// surviving a build-change settings reset, migration from a board a per-hand
// build (EE_HANDS + INIT_EE_HANDS_*) left, power cuts during a write, host
// writes, and the pairing report (both halves claiming the same side).
//
// The board is modeled as two side sectors, the settings (the EECONFIG magic
// and handedness byte at offset 14) and the build reset record, booted the
// way svalboard.c does: strap probe, sval_side_init(), build_reset_pre_init().

#include <stdio.h>
#include <string.h>
#include "side.h"
#include "build_reset.h"

static int checks, failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            failures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

#define STRAP_DEFAULT 0
#define STRAP_LEFT 1
#define STRAP_ANOMALY 2
#define STRAP_RIGHT 4

// ---- the board ----------------------------------------------------------------------

static sval_side_record_t   sector[2]; // A, B
static build_reset_record_t br;
static uint16_t             ee_magic;
static uint8_t              ee_hand;
static bool                 cut_next_write; // the next side write loses power mid-erase

static void erase_all(void) {
    memset(sector, 0xFF, sizeof(sector));
    memset(&br, 0xFF, sizeof(br));
    ee_magic = 0xFFFF;
    ee_hand  = 0xFF;
}

static uint8_t side_now, source_now;

static void side_write(uint8_t side, uint8_t source) {
    bool                      write_b;
    const sval_side_record_t *n = sval_side_newest(&sector[0], &sector[1], &write_b);
    sval_side_record_t        r;
    sval_side_record_make(&r, side, source, n ? n->seq + 1 : 1);
    if (cut_next_write) {
        cut_next_write = false;
        memset(&sector[write_b], 0xFF, sizeof(r)); // erased, never programmed
        return;
    }
    sector[write_b] = r;
}

// What QMK's eeconfig_init() leaves: the settings formatted, the magic set,
// the handedness byte as INIT_EE_HANDS_* (per-hand builds) or zero (either builds).
static void eeconfig_init(int per_hand_left) {
    ee_magic = 0xFEE2;
    ee_hand  = per_hand_left < 0 ? 0 : (uint8_t)per_hand_left;
}

// One boot of an either-hand build with image hash `image`.
static void boot_either(uint8_t strap, uint32_t image) {
    bool                      write_b;
    const sval_side_record_t *rec      = sval_side_newest(&sector[0], &sector[1], &write_b);
    uint8_t                   migrated = rec ? SVAL_SIDE_UNKNOWN : sval_side_from_eeconfig(ee_magic, ee_hand);
    sval_side_decision_t      d;
    sval_side_decide(sval_side_from_strap(strap), rec, migrated, &d);
    side_now   = d.side;
    source_now = d.source;
    if (d.write) side_write(d.write_side, d.write_source);
    // build_reset_pre_init() / quantum_init() / build_reset_post_init()
    if (build_reset_due(true, &br, image)) {
        eeconfig_init(-1);
        build_reset_record_make(&br, image, BUILD_RESET_NOTICE);
    }
}

// One boot of a per-hand build: INIT_EE_HANDS_* writes the handedness byte.
static void boot_per_hand(bool left, uint32_t image) {
    if (build_reset_due(true, &br, image) || ee_magic != 0xFEE2) {
        eeconfig_init(left);
        build_reset_record_make(&br, image, BUILD_RESET_NOTICE);
    }
    ee_hand = left ? 1 : 0;
}

// ---- tests --------------------------------------------------------------------------

static void test_record(void) {
    sval_side_record_t r;
    sval_side_record_make(&r, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST, 7);
    CHECK(sizeof(r) == 16);
    CHECK(sval_side_record_valid(&r));
    sval_side_record_t bad = r;
    bad.side ^= 1; // the CRC no longer matches
    CHECK(!sval_side_record_valid(&bad));
    bad         = r;
    bad.version = 2;
    CHECK(!sval_side_record_valid(&bad));
    sval_side_record_make(&bad, 2, SVAL_SIDE_SRC_HOST, 1); // not a side
    CHECK(!sval_side_record_valid(&bad));
    sval_side_record_make(&bad, SVAL_SIDE_LEFT, 9, 1); // not a source
    CHECK(!sval_side_record_valid(&bad));
    CHECK(!sval_side_record_valid(NULL));
    memset(&bad, 0xFF, sizeof(bad)); // erased flash
    CHECK(!sval_side_record_valid(&bad));
    CHECK(sval_side_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u); // CRC-32 check value
}

static void test_copies(void) {
    erase_all();
    side_write(SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST);  // into A
    side_write(SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_HOST); // into B
    bool                      wb;
    const sval_side_record_t *n = sval_side_newest(&sector[0], &sector[1], &wb);
    CHECK(n == &sector[1] && n->side == SVAL_SIDE_RIGHT && n->seq == 2 && !wb);
    // A power cut while writing the next copy leaves the newer one.
    cut_next_write = true;
    side_write(SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST);
    n = sval_side_newest(&sector[0], &sector[1], &wb);
    CHECK(n && n->side == SVAL_SIDE_RIGHT && n->seq == 2);
    side_write(SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST);
    n = sval_side_newest(&sector[0], &sector[1], &wb);
    CHECK(n && n->side == SVAL_SIDE_LEFT && n->seq == 3);
}

static void test_priority(void) {
    sval_side_record_t   rec;
    sval_side_decision_t d;
    // Strap first, and it rewrites a record that disagrees.
    sval_side_record_make(&rec, SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_HOST, 1);
    sval_side_decide(SVAL_SIDE_LEFT, &rec, SVAL_SIDE_UNKNOWN, &d);
    CHECK(d.side == SVAL_SIDE_LEFT && d.source == SVAL_SIDE_SRC_STRAP && d.write && d.write_side == SVAL_SIDE_LEFT && d.write_source == SVAL_SIDE_SRC_STRAP);
    // A record that agrees with the strap is left alone.
    sval_side_record_make(&rec, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST, 1);
    sval_side_decide(SVAL_SIDE_LEFT, &rec, SVAL_SIDE_UNKNOWN, &d);
    CHECK(d.side == SVAL_SIDE_LEFT && d.source == SVAL_SIDE_SRC_STRAP && !d.write);
    // No strap: the record, and migration is not looked at.
    sval_side_record_make(&rec, SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_FACTORY, 1);
    sval_side_decide(SVAL_SIDE_UNKNOWN, &rec, SVAL_SIDE_LEFT, &d);
    CHECK(d.side == SVAL_SIDE_RIGHT && d.source == SVAL_SIDE_SRC_FACTORY && !d.write);
    // A cleared record: unknown, and no migration.
    sval_side_record_make(&rec, SVAL_SIDE_UNKNOWN, SVAL_SIDE_SRC_NONE, 2);
    sval_side_decide(SVAL_SIDE_UNKNOWN, &rec, SVAL_SIDE_LEFT, &d);
    CHECK(d.side == SVAL_SIDE_UNKNOWN && !d.write);
    // No record: migration, written down.
    sval_side_decide(SVAL_SIDE_UNKNOWN, NULL, SVAL_SIDE_RIGHT, &d);
    CHECK(d.side == SVAL_SIDE_RIGHT && d.source == SVAL_SIDE_SRC_MIGRATED && d.write && d.write_side == SVAL_SIDE_RIGHT);
    // Nothing at all: unknown, and an UNKNOWN record so the next boot does not look again.
    sval_side_decide(SVAL_SIDE_UNKNOWN, NULL, SVAL_SIDE_UNKNOWN, &d);
    CHECK(d.side == SVAL_SIDE_UNKNOWN && d.source == SVAL_SIDE_SRC_NONE && d.write && d.write_side == SVAL_SIDE_UNKNOWN);
    // The strap's classes: only LEFT and RIGHT are sides.
    CHECK(sval_side_from_strap(STRAP_LEFT) == SVAL_SIDE_LEFT);
    CHECK(sval_side_from_strap(STRAP_RIGHT) == SVAL_SIDE_RIGHT);
    CHECK(sval_side_from_strap(STRAP_DEFAULT) == SVAL_SIDE_UNKNOWN);
    CHECK(sval_side_from_strap(STRAP_ANOMALY) == SVAL_SIDE_UNKNOWN);
    CHECK(sval_side_from_strap(3) == SVAL_SIDE_UNKNOWN && sval_side_from_strap(0xFF) == SVAL_SIDE_UNKNOWN);
}

static void test_eeconfig(void) {
    CHECK(sval_side_from_eeconfig(0xFEE2, 1) == SVAL_SIDE_LEFT);
    CHECK(sval_side_from_eeconfig(0xFEE2, 0) == SVAL_SIDE_RIGHT);
    CHECK(sval_side_from_eeconfig(0xB1A0, 1) == SVAL_SIDE_LEFT);
    CHECK(sval_side_from_eeconfig(0xFEE3, 0) == SVAL_SIDE_RIGHT);
    CHECK(sval_side_from_eeconfig(0xFFFF, 1) == SVAL_SIDE_UNKNOWN); // disabled (a reset is pending)
    CHECK(sval_side_from_eeconfig(0x0000, 0) == SVAL_SIDE_UNKNOWN); // blank or foreign settings
    CHECK(sval_side_from_eeconfig(0xFEE6, 1) == SVAL_SIDE_UNKNOWN);
    CHECK(sval_side_from_eeconfig(0xFEE2, 2) == SVAL_SIDE_UNKNOWN);
    CHECK(sval_side_from_eeconfig(0xFEE2, 0xFF) == SVAL_SIDE_UNKNOWN);
}

// A field board on today's per-hand images moves to the single image.
static void test_migration(void) {
    for (int left = 0; left < 2; left++) {
        erase_all();
        boot_per_hand(left, 0x1111);
        boot_per_hand(left, 0x1111);
        boot_per_hand(left, 0x2222); // an update between per-hand releases
        // The first boot of the either image: adopted before the build reset formats the settings.
        boot_either(STRAP_DEFAULT, 0xE1);
        uint8_t want = left ? SVAL_SIDE_LEFT : SVAL_SIDE_RIGHT;
        CHECK(side_now == want && source_now == SVAL_SIDE_SRC_MIGRATED);
        CHECK(ee_hand == 0); // the reset did format them
        // Every later boot, and a later either release (another build reset), keeps it.
        boot_either(STRAP_DEFAULT, 0xE1);
        CHECK(side_now == want && source_now == SVAL_SIDE_SRC_MIGRATED);
        boot_either(STRAP_DEFAULT, 0xE2);
        CHECK(side_now == want);
        // Back to a per-hand image for the other side and forward again: the
        // record wins (only a host write or a strap changes it).
        boot_per_hand(!left, 0x3333);
        boot_either(STRAP_DEFAULT, 0xE3);
        CHECK(side_now == want);
    }
}

// A factory-fresh board, and a cleared record: unknown, never the formatted byte.
static void test_unknown(void) {
    erase_all();
    boot_either(STRAP_DEFAULT, 0xE1);
    CHECK(side_now == SVAL_SIDE_UNKNOWN && source_now == SVAL_SIDE_SRC_NONE);
    // The build reset set the magic with a zero handedness byte, which would read as RIGHT;
    // the UNKNOWN record keeps the next boots (and the next release) from looking at it.
    CHECK(sval_side_from_eeconfig(ee_magic, ee_hand) == SVAL_SIDE_RIGHT);
    boot_either(STRAP_DEFAULT, 0xE1);
    CHECK(side_now == SVAL_SIDE_UNKNOWN);
    boot_either(STRAP_DEFAULT, 0xE2);
    CHECK(side_now == SVAL_SIDE_UNKNOWN);
    // The host sets it.
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_FACTORY) == SVAL_SIDE_OK);
    side_write(SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_FACTORY);
    boot_either(STRAP_DEFAULT, 0xE2);
    CHECK(side_now == SVAL_SIDE_RIGHT && source_now == SVAL_SIDE_SRC_FACTORY);
    // Cleared: unknown again, through a build reset too.
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_UNKNOWN, SVAL_SIDE_SRC_NONE) == SVAL_SIDE_OK);
    side_write(SVAL_SIDE_UNKNOWN, SVAL_SIDE_SRC_NONE);
    boot_either(STRAP_DEFAULT, 0xE3);
    CHECK(side_now == SVAL_SIDE_UNKNOWN && source_now == SVAL_SIDE_SRC_NONE);
}

static void test_strap(void) {
    // A strap wins over the record and rewrites it, then the record follows.
    erase_all();
    side_write(SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_HOST);
    boot_either(STRAP_LEFT, 0xE1);
    CHECK(side_now == SVAL_SIDE_LEFT && source_now == SVAL_SIDE_SRC_STRAP);
    bool                      wb;
    const sval_side_record_t *n = sval_side_newest(&sector[0], &sector[1], &wb);
    CHECK(n && n->side == SVAL_SIDE_LEFT && n->source == SVAL_SIDE_SRC_STRAP);
    // The same thumb on a board that migrated another side: the strap wins.
    erase_all();
    boot_per_hand(false, 0x1111);
    boot_either(STRAP_LEFT, 0xE1);
    CHECK(side_now == SVAL_SIDE_LEFT && source_now == SVAL_SIDE_SRC_STRAP);
    // A right strap.
    erase_all();
    boot_either(STRAP_RIGHT, 0xE1);
    CHECK(side_now == SVAL_SIDE_RIGHT && source_now == SVAL_SIDE_SRC_STRAP);
    // A thumb moved off: the record it left stays.
    boot_either(STRAP_DEFAULT, 0xE1);
    CHECK(side_now == SVAL_SIDE_RIGHT && source_now == SVAL_SIDE_SRC_STRAP);
}

static void test_host_check(void) {
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST) == SVAL_SIDE_OK);
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_FACTORY) == SVAL_SIDE_OK);
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, 2, SVAL_SIDE_SRC_HOST) == SVAL_SIDE_INVALID);
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_MIGRATED) == SVAL_SIDE_INVALID); // only firmware migrates
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_STRAP) == SVAL_SIDE_INVALID);
    CHECK(sval_side_host_check(SVAL_SIDE_UNKNOWN, SVAL_SIDE_UNKNOWN, SVAL_SIDE_SRC_HOST) == SVAL_SIDE_INVALID); // a clear has no source
    // The strap wins: a host write that disagrees, or a clear, is refused.
    CHECK(sval_side_host_check(SVAL_SIDE_LEFT, SVAL_SIDE_RIGHT, SVAL_SIDE_SRC_HOST) == SVAL_SIDE_STRAP);
    CHECK(sval_side_host_check(SVAL_SIDE_LEFT, SVAL_SIDE_UNKNOWN, SVAL_SIDE_SRC_NONE) == SVAL_SIDE_STRAP);
    CHECK(sval_side_host_check(SVAL_SIDE_LEFT, SVAL_SIDE_LEFT, SVAL_SIDE_SRC_HOST) == SVAL_SIDE_OK);
}

static void test_pair(void) {
    CHECK(sval_side_pair(SVAL_SIDE_LEFT, false, false, SVAL_SIDE_UNKNOWN) == SVAL_SIDE_PAIR_OK);
    CHECK(sval_side_pair(SVAL_SIDE_LEFT, true, false, SVAL_SIDE_UNKNOWN) == SVAL_SIDE_PAIR_PENDING);
    CHECK(sval_side_pair(SVAL_SIDE_LEFT, true, true, SVAL_SIDE_RIGHT) == SVAL_SIDE_PAIR_OK);
    CHECK(sval_side_pair(SVAL_SIDE_RIGHT, true, true, SVAL_SIDE_LEFT) == SVAL_SIDE_PAIR_OK);
    // Both halves claim the same side: reported, never resolved.
    CHECK(sval_side_pair(SVAL_SIDE_LEFT, true, true, SVAL_SIDE_LEFT) == SVAL_SIDE_PAIR_SAME);
    CHECK(sval_side_pair(SVAL_SIDE_RIGHT, true, true, SVAL_SIDE_RIGHT) == SVAL_SIDE_PAIR_SAME);
    CHECK(sval_side_pair(SVAL_SIDE_LEFT, true, true, SVAL_SIDE_UNKNOWN) == SVAL_SIDE_PAIR_UNKNOWN);
    CHECK(sval_side_pair(SVAL_SIDE_UNKNOWN, true, true, SVAL_SIDE_RIGHT) == SVAL_SIDE_PAIR_UNKNOWN);
    CHECK(sval_side_pair(SVAL_SIDE_UNKNOWN, true, true, SVAL_SIDE_UNKNOWN) == SVAL_SIDE_PAIR_UNKNOWN);
}

// ---- a Vial board's old store (QMK wear leveling, 2 MB layout) ---------------------

static uint8_t vial_store[SVAL_SIDE_VIAL_STORE_BACKING]; // backing bytes, un-inverted
static uint16_t vial_word(uint32_t off, void *ctx) {
    (void)ctx;
    return (uint16_t)(vial_store[off] | (vial_store[off + 1] << 8));
}
static void vial_consolidated(const uint8_t *logical16, bool valid_sum) {
    memset(vial_store, 0, sizeof(vial_store)); // erased flash, un-inverted: zeros, an empty log
    memcpy(vial_store, logical16, 16);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < SVAL_SIDE_VIAL_STORE_LOGICAL; i++) h = (h ^ vial_store[i]) * 0x100000001b3ull;
    if (!valid_sum) h ^= 1;
    memcpy(&vial_store[SVAL_SIDE_VIAL_STORE_LOGICAL], &h, 8);
}
static uint32_t vial_log_at;
static void vial_log(const uint8_t *e, uint32_t n) { // n bytes of one entry (2, 4, 6 or 8)
    memcpy(&vial_store[vial_log_at], e, n);
    vial_log_at += n;
}

static void test_vial_store(void) {
    const uint8_t left[16]  = {0xE3, 0xFE, [14] = 1};
    const uint8_t right[16] = {0xE3, 0xFE, [14] = 0};
    // The consolidated image after a consolidation: Vial's magic and the side.
    vial_consolidated(left, true);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_LEFT);
    vial_consolidated(right, true);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_RIGHT);
    // A checksum that fails: QMK starts from zeros, so no side.
    vial_consolidated(left, false);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_UNKNOWN);
    // Never consolidated: the magic as a multi-byte entry, the side as a byte below 64.
    const uint8_t none[16] = {0};
    vial_consolidated(none, false);
    vial_log_at = SVAL_SIDE_VIAL_STORE_LOGICAL + 8;
    const uint8_t magic[4] = {(0 << 6) | (2 << 3) | 0, 0x00, 0x00, 0xE3}, magic_hi[2] = {0xFE, 0x00};
    vial_log(magic, 4);
    vial_log(magic_hi, 2);
    const uint8_t hand_left[2] = {(1 << 6) | 14, 1};
    vial_log(hand_left, 2);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_LEFT);
    // A later write wins: the side rewritten as a 0/1 word (address 14, value 0) is right.
    const uint8_t hand_word0[2] = {(2 << 6) | (0 << 5) | (14 >> 9), 14 >> 1};
    vial_log(hand_word0, 2);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_RIGHT);
    // Another magic (this firmware's, or none at all) is never taken as Vial's side.
    const uint8_t other[16] = {0xA0, 0xB1, [14] = 2};
    vial_consolidated(other, true);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_UNKNOWN);
    memset(vial_store, 0, sizeof(vial_store));
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_UNKNOWN);
    // A corrupt entry ends the replay where QMK's would, keeping what came before.
    vial_consolidated(left, true);
    vial_log_at = SVAL_SIDE_VIAL_STORE_LOGICAL + 8;
    const uint8_t bad[2] = {0xC0, 0x00}; // type 3 does not exist
    vial_log(bad, 2);
    vial_log(hand_word0, 2);
    CHECK(sval_side_from_vial_store(vial_word, NULL) == SVAL_SIDE_LEFT);
}

int main(void) {
    test_vial_store();
    test_record();
    test_copies();
    test_priority();
    test_eeconfig();
    test_migration();
    test_unknown();
    test_strap();
    test_host_check();
    test_pair();
    printf("test_side: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
