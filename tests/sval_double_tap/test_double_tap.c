// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later

// Host test for the double-tap RESET state (keyboards/svalboard/double_tap_reset.h):
// a boot sequence of resets, each RUN pin, watchdog, power-up or debugger,
// enters the bootloader exactly when two RUN-pin resets follow each other
// inside one boot's window, and never after a software reset.
#include <stdio.h>
#include <stdlib.h>
#include "double_tap_reset.h"

#define HAD_POR 0x00000100u
#define HAD_PSM 0x00100000u

static int failures;
#define CHECK(c)                                                           \
    do {                                                                   \
        if (!(c)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

typedef enum { RUN_PIN, WD_FORCE, WD_TIMER, POWER_UP, DEBUGGER, RUN_WITH_POR } reset_t;

// The reset reason registers each kind of reset leaves. A RUN-pin reset or a
// power-up resets the watchdog block, so its reason reads 0; a watchdog
// reset leaves CHIP_RESET as the last chip-level reset had it.
static void regs_for(reset_t r, uint32_t *wd, uint32_t *chip) {
    switch (r) {
        case RUN_PIN:
            *wd = 0, *chip = DTR_CHIP_HAD_RUN;
            break;
        case WD_FORCE:
            *wd = DTR_WD_REASON_FORCE, *chip = DTR_CHIP_HAD_RUN;
            break;
        case WD_TIMER:
            *wd = DTR_WD_REASON_TIMER, *chip = DTR_CHIP_HAD_RUN;
            break;
        case POWER_UP:
            *wd = 0, *chip = HAD_POR;
            break;
        case DEBUGGER:
            *wd = 0, *chip = HAD_PSM;
            break;
        case RUN_WITH_POR:
            *wd = 0, *chip = DTR_CHIP_HAD_RUN | HAD_POR;
            break;
    }
}

// One boot from reset r with the word as the last boot left it. in_window:
// the next reset comes before this boot's window ends. Returns whether this
// boot enters the bootloader; *word is what the next boot finds.
static bool boot(uint32_t *word, reset_t r, bool in_window) {
    uint32_t wd, chip;
    regs_for(r, &wd, &chip);
    *word = dtr_early_step(*word, dtr_is_tap(wd, chip));
    if (dtr_late_fires(*word)) {
        *word = 0;
        return true;
    }
    if (!in_window) *word = 0; // the window ran out
    return false;
}

// Runs a sequence of resets; returns the index of the boot that enters the
// bootloader, or -1. gaps[i]: reset i+1 lands inside boot i's window.
static int run(uint32_t word, const reset_t *rs, const bool *gaps, int n) {
    for (int i = 0; i < n; i++)
        if (boot(&word, rs[i], i + 1 < n && gaps[i])) return i;
    return -1;
}

int main(void) {
    // The values.
    CHECK(dtr_early_step(0, true) == DTR_ARMED);
    CHECK(dtr_early_step(DTR_ARMED, true) == DTR_FIRE);
    CHECK(dtr_early_step(DTR_FIRE, true) == DTR_ARMED);
    CHECK(dtr_early_step(0x12345678u, true) == DTR_ARMED);
    CHECK(dtr_early_step(DTR_ARMED, false) == 0);
    CHECK(dtr_early_step(DTR_FIRE, false) == 0);
    CHECK(!dtr_late_fires(DTR_ARMED) && dtr_late_fires(DTR_FIRE) && !dtr_late_fires(0));

    // Which resets count.
    uint32_t wd, chip;
    regs_for(RUN_PIN, &wd, &chip);
    CHECK(dtr_is_tap(wd, chip));
    regs_for(RUN_WITH_POR, &wd, &chip);
    CHECK(dtr_is_tap(wd, chip));
    regs_for(WD_FORCE, &wd, &chip);
    CHECK(!dtr_is_tap(wd, chip));
    regs_for(WD_TIMER, &wd, &chip);
    CHECK(!dtr_is_tap(wd, chip));
    regs_for(POWER_UP, &wd, &chip);
    CHECK(!dtr_is_tap(wd, chip));
    regs_for(DEBUGGER, &wd, &chip);
    CHECK(!dtr_is_tap(wd, chip));
    CHECK(!dtr_is_tap(DTR_WD_REASON_FORCE | DTR_WD_REASON_TIMER, DTR_CHIP_HAD_RUN));

    // A quick double tap enters, on the second tap.
    {
        reset_t r[] = {RUN_PIN, RUN_PIN};
        bool    g[] = {true};
        CHECK(run(0, r, g, 2) == 1);
    }
    // Two taps too far apart do not.
    {
        reset_t r[] = {RUN_PIN, RUN_PIN};
        bool    g[] = {false};
        CHECK(run(0, r, g, 2) == -1);
    }
    // A single press never does, whatever an earlier boot left in the word
    // (the window clears ARMED, so ARMED is never stale).
    for (int i = 0; i < 1000; i++) {
        reset_t r[] = {RUN_PIN};
        bool    g[] = {false};
        CHECK(run(i % 3 == 0 ? 0 : i % 3 == 1 ? DTR_FIRE : 0x9E3779B9u * (uint32_t)i, r, g, 1) == -1);
    }
    // Software resets, however quick, never enter, alone, mixed or after a tap.
    reset_t soft[] = {WD_FORCE, WD_TIMER, POWER_UP, DEBUGGER};
    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            reset_t r1[] = {soft[a], soft[b]};
            reset_t r2[] = {RUN_PIN, soft[b]};
            reset_t r3[] = {soft[a], RUN_PIN};
            bool    g[]  = {true};
            CHECK(run(0, r1, g, 2) == -1);
            CHECK(run(DTR_ARMED, r1, g, 2) == -1);
            CHECK(run(DTR_FIRE, r1, g, 2) == -1);
            CHECK(run(0, r2, g, 2) == -1); // a tap, then an updater reboot inside its window
            CHECK(run(0, r3, g, 2) == -1); // a reboot, then a single tap
        }
    }
    // A tap, a software reboot inside its window, a tap: the reboot breaks the pair.
    {
        reset_t r[] = {RUN_PIN, WD_FORCE, RUN_PIN};
        bool    g[] = {true, true};
        CHECK(run(0, r, g, 3) == -1);
    }
    // After a software reboot the next two quick taps still enter.
    {
        reset_t r[] = {WD_FORCE, RUN_PIN, RUN_PIN};
        bool    g[] = {false, true};
        CHECK(run(0, r, g, 3) == 2);
    }
    // A stale value at a new image's word: a watchdog reset (the commit, the
    // boot ROM after a UF2 copy) or a power-up clears it before it is read.
    reset_t clears[] = {WD_FORCE, WD_TIMER, POWER_UP, DEBUGGER};
    for (int a = 0; a < 4; a++) {
        reset_t r[] = {clears[a]};
        bool    g[] = {false};
        CHECK(run(DTR_ARMED, r, g, 1) == -1);
        CHECK(run(DTR_FIRE, r, g, 1) == -1);
    }
    // Three quick taps: in on the second; the bootloader then owns the chip.
    {
        reset_t r[] = {RUN_PIN, RUN_PIN, RUN_PIN};
        bool    g[] = {true, true};
        CHECK(run(0, r, g, 3) == 1);
    }

    if (failures) {
        fprintf(stderr, "test_double_tap: %d failures\n", failures);
        return 1;
    }
    printf("test_double_tap: ok\n");
    return 0;
}
