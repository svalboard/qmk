// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host test for the build reset log (keyboards/svalboard/build_reset.c, the
// slots of build_reset.h) on a simulated sector, with a power cut at every
// write and in several ways a page program can be cut short:
//   - the host's acknowledgement of the notice (0x2A) never puts the record
//     at risk: after a cut anywhere in it the next boot keeps the settings;
//   - a blank reset (SETTINGS_RESET mode 1) cut anywhere while its record is
//     written comes back blank, never as the default keymap;
//   - a reset to defaults cut short never comes back blank, and older
//     firmware (which reads slot 0 only) never finds a blank request there.

#include <stdio.h>
#include <string.h>
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

#define SECTOR_BYTES (BUILD_RESET_SLOTS * BUILD_RESET_PAGE_BYTES)

// ---- the simulated flash ----------------------------------------------------------

static uint8_t sector[SECTOR_BYTES];
static uint8_t page[BUILD_RESET_PAGE_BYTES];
static int     writes_left = -1; // writes before the power cut; -1: none
static int     tear;             // how the write that the power cut stops is left
static bool    power_off;        // the power went: nothing more is written
static bool    stuck;            // programs and rewrites do nothing (a worn sector)

enum { TEAR_NONE, TEAR_EVEN, TEAR_ODD, TEAR_FIRST_HALF, TEAR_ALL_BUT_FIRST, TEAR_MODES };

static bool torn_takes(int i) {
    switch (tear) {
        case TEAR_NONE:
            return false;
        case TEAR_EVEN:
            return !(i & 1);
        case TEAR_ODD:
            return i & 1;
        case TEAR_FIRST_HALF:
            return i < (int)BUILD_RESET_PAGE_BYTES / 2;
        default:
            return i != 0;
    }
}

// False when the power goes during this write.
static bool write_whole(void) {
    if (writes_left == 0) {
        power_off = true;
        return false;
    }
    if (writes_left > 0) writes_left--;
    return true;
}

static void sim_program(uint32_t off) {
    CHECK(off % BUILD_RESET_PAGE_BYTES == 0 && off < SECTOR_BYTES);
    if (power_off) return;
    bool whole = write_whole();
    if (stuck) return;
    for (int i = 0; i < (int)BUILD_RESET_PAGE_BYTES; i++)
        if (whole || torn_takes(i)) sector[off + i] &= page[i];
}

static bool sim_rewrite(const void *data, uint32_t len) {
    if (power_off) return false;
    bool whole = write_whole();
    if (stuck) return false;
    memset(sector, 0xFF, sizeof(sector)); // the erase comes first; a cut after it leaves it erased
    for (uint32_t i = 0; i < len; i++)
        if (whole || torn_takes((int)i)) sector[i] &= ((const uint8_t *)data)[i];
    return whole;
}

static const build_reset_flash_t flash = {page, sim_program, sim_rewrite};

static void power_on(void) {
    writes_left = -1;
    power_off   = false;
}

static void cut_after(int n, int how) {
    writes_left = n;
    tear        = how;
    power_off   = false;
}

// ---- the board -----------------------------------------------------------------

enum { SETTINGS_CUSTOM, SETTINGS_DEFAULTS, SETTINGS_BLANK };
static int  settings;
static bool notice;

// One boot of image, as build_reset.c: pre-init decides, quantum_init resets,
// post-init blanks the keymap, then records. True if it reset.
static bool boot(uint32_t image) {
    build_reset_state_t st;
    build_reset_state(sector, image, &st);
    notice = st.notice;
    if (!st.pending) return false;
    settings = st.blank_due ? SETTINGS_BLANK : SETTINGS_DEFAULTS;
    if (build_reset_log_record(sector, &flash, image, BUILD_RESET_NOTICE, st.blank_due)) notice = true;
    return true;
}

// Boots until a boot keeps the settings (a few at most).
static void boot_until_kept(uint32_t image) {
    for (int i = 0; i < 4 && boot(image); i++) {
    }
}

// What firmware from before the log reads: slot 0, with its blank rule (a
// cleared record and a whole mark).
static bool old_firmware_blanks(void) {
    build_reset_record_t rec;
    memcpy(&rec, sector, sizeof(rec));
    uint32_t m0, m1;
    memcpy(&m0, sector + 16, 4);
    memcpy(&m1, sector + 20, 4);
    return build_reset_cleared(&rec) && m0 == BUILD_RESET_BLANK_MAGIC && m1 == ~BUILD_RESET_BLANK_MAGIC;
}

static void fresh(void) {
    power_on();
    stuck = false;
    memset(sector, 0xFF, sizeof(sector));
}

// The board after n blank resets of image and the host's acknowledgement,
// with the settings customized afterwards.
static void board_after_blank_resets(uint32_t image, int n) {
    fresh();
    boot_until_kept(image);
    for (int i = 0; i < n; i++) {
        CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_BLANK));
        boot_until_kept(image);
        CHECK(settings == SETTINGS_BLANK);
    }
    CHECK(build_reset_log_ack(sector, &flash));
    settings = SETTINGS_CUSTOM;
}

int main(void) {
    const uint32_t A = 0x1234567u, B = 0x7654321u;

    // Upgrade: a record that firmware from before the log wrote (slot 0, the
    // rest erased) reads as before.
    fresh();
    build_reset_record_t rec;
    build_reset_record_make(&rec, A, BUILD_RESET_NOTICE);
    memcpy(sector, &rec, sizeof(rec));
    CHECK(build_reset_slot(sector) == 0);
    CHECK(!boot(A) && notice);
    CHECK(boot(B) && settings == SETTINGS_DEFAULTS && notice);
    CHECK(build_reset_slot(sector) == 0); // a reset to defaults starts the log again
    fresh();
    CHECK(build_reset_slot(sector) == -1);
    CHECK(boot(A) && !boot(A) && notice);

    // ---- the acknowledgement (S9) ----
    // Done: the notice stays clear across boots, and nothing resets.
    for (int n = 0; n < 3; n++) {
        board_after_blank_resets(A, n);
        CHECK(!boot(A) && !notice && settings == SETTINGS_CUSTOM);
        CHECK(!boot(A) && !notice);
    }
    // A cut at the write, each way: the settings stay; the notice reads as
    // before or cleared, and the host sends 0x2A again.
    for (int n = 0; n < 3; n++) {
        for (int how = 0; how < TEAR_MODES; how++) {
            board_after_blank_resets(A, n);
            CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_DEFAULTS));
            boot_until_kept(A);
            settings = SETTINGS_CUSTOM; // the layout reloaded, then 0x2A
            CHECK(notice);
            cut_after(0, how);
            build_reset_log_ack(sector, &flash);
            power_on();
            CHECK(!boot(A) && settings == SETTINGS_CUSTOM);
            CHECK(build_reset_log_ack(sector, &flash));
            CHECK(!boot(A) && !notice && settings == SETTINGS_CUSTOM);
        }
    }
    // A program that does not take: the notice stays, and the record too.
    board_after_blank_resets(A, 0);
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_DEFAULTS));
    boot_until_kept(A);
    settings = SETTINGS_CUSTOM;
    stuck    = true;
    CHECK(!build_reset_log_ack(sector, &flash));
    stuck = false;
    CHECK(!boot(A) && notice && settings == SETTINGS_CUSTOM);

    // ---- the blank reset (S11) ----
    // From several places in the log, a cut at each write of the boot that
    // records the blank reset, each way: the board comes back blank.
    const int starts[] = {0, 1, 5, (int)BUILD_RESET_SLOTS - 3};
    for (size_t s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
        for (int k = 0; k < 6; k++) {
            for (int how = 0; how < TEAR_MODES; how++) {
                board_after_blank_resets(A, starts[s]);
                CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_BLANK));
                cut_after(k, how);
                CHECK(boot(A) && settings == SETTINGS_BLANK);
                bool cut = power_off;
                power_on();
                if (!cut) settings = SETTINGS_CUSTOM; // recorded: the user sets it up again
                boot_until_kept(A);
                CHECK(cut ? settings == SETTINGS_BLANK : settings == SETTINGS_CUSTOM);
                CHECK(!boot(A) && notice);
                // (a cut at the write that zeroes it may leave it: older
                // firmware would then blank the board on a downgrade)
                if (!cut) CHECK(!old_firmware_blanks());
            }
        }
    }
    // Blank resets past the last slot: each still works (erasing first when full).
    board_after_blank_resets(A, (int)BUILD_RESET_SLOTS + 2);
    CHECK(!boot(A) && settings == SETTINGS_CUSTOM);
    // A blank reset after a blank reset (that slot's mark bytes are zeroed).
    board_after_blank_resets(A, 1);
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_BLANK));
    CHECK(boot(A) && settings == SETTINGS_BLANK);
    // Two SETTINGS_RESETs before the restart: the last one asked wins.
    board_after_blank_resets(A, 1);
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_DEFAULTS));
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_BLANK));
    CHECK(boot(A) && settings == SETTINGS_BLANK);
    board_after_blank_resets(A, 1);
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_BLANK));
    CHECK(build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_DEFAULTS));
    CHECK(boot(A) && settings == SETTINGS_DEFAULTS);

    // A reset to defaults cut short, after blank resets: never blank. Either
    // the request did not take (no reset) or the reset is to defaults.
    for (int n = 0; n < 3; n++) {
        for (int how = 0; how < TEAR_MODES; how++) {
            board_after_blank_resets(A, n);
            cut_after(0, how);
            build_reset_log_invalidate(sector, &flash, BUILD_RESET_TO_DEFAULTS);
            power_on();
            boot_until_kept(A);
            CHECK(settings == SETTINGS_CUSTOM || settings == SETTINGS_DEFAULTS);
            CHECK(!old_firmware_blanks());
        }
    }
    // A different build after blank resets: defaults, and the log starts again.
    board_after_blank_resets(A, 3);
    CHECK(boot(B) && settings == SETTINGS_DEFAULTS && build_reset_slot(sector) == 0);

    // A cut while a reset to defaults is recorded only repeats that reset.
    for (int how = 0; how < TEAR_MODES; how++) {
        fresh();
        cut_after(0, how);
        CHECK(boot(A));
        power_on();
        settings = SETTINGS_CUSTOM;
        // (TEAR_FIRST_HALF: the record's bytes were all programmed before the cut)
        bool again = boot(A);
        CHECK(again == (how != TEAR_FIRST_HALF));
        CHECK(settings == (again ? SETTINGS_DEFAULTS : SETTINGS_CUSTOM));
        CHECK(!boot(A));
    }

    printf("build_reset_log: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
