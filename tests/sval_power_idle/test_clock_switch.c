// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
// Host test for the deep-idle clock switch in keyboards/svalboard/power.c: a PIO
// state machine (the split serial) keeps its rate at every step of the switch
// that anything else could run in. The clk_sys switch and the new dividers go
// in back to back with interrupts off, and the slow steps (the system PLL,
// clk_peri) come before or after, never between them. Built with mock pico-sdk
// headers (stub/) by util/updater_test/run.sh, from a copy of power.c in its
// own directory, so its "svalboard.h" is the mock.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "quantum.h"
#include "svalboard.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/structs/clocks.h"

static int checks, failures;
#define CHECK(c)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(c)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                       \
    } while (0)

// ---- the mocks ------------------------------------------------------------------

saved_values_t global_saved_values;
pio_hw_t       host_pio[2];
clocks_hw_t    host_clocks;
struct host_pll {
    int unused;
} host_pll_sys;

static uint32_t now_ms = 1000;
uint32_t        timer_read32(void) {
    return now_ms;
}
uint32_t timer_elapsed32(uint32_t t) {
    return now_ms - t;
}
bool is_keyboard_master(void) {
    return false; // the half without USB: its SlaveThread answers the other half
}

#define FULL_HZ 125000000u
#define SM_HZ 8000000u // the split serial's state machine: 8 cycles a bit at 1 Mbaud

static uint32_t clk_sys_hz = FULL_HZ;
static int      irq_off;          // save_and_disable_interrupts depth
static int      steps;            // mock calls during the switch
static int      steps_wrong;      // ...entered while the state machine ran at the wrong rate
static int      clk_sys_irq_on;   // clk_sys switches made with interrupts on
static int      clk_sys_switches; // clk_sys switches
static int      pll_inits, pll_deinits, clk_peri_sets;
static char     wrong_at[128]; // which steps found the wrong rate

// The split serial's rate now: clk_sys / its divider (16.8 fixed point in bits 31:8).
static bool sm_rate_right(void) {
    uint32_t div256 = host_pio[0].sm[0].clkdiv >> 8;
    uint64_t hz     = (uint64_t)clk_sys_hz * 256u / div256;
    return hz > SM_HZ - SM_HZ / 200 && hz < SM_HZ + SM_HZ / 200; // within 0.5%
}

// Every mock call is a point where time passes, or where an interrupt or the
// SlaveThread could run: the state machine has to be at its rate there.
static void step(const char *what) {
    steps++;
    if (!sm_rate_right()) {
        steps_wrong++;
        if (strlen(wrong_at) + strlen(what) + 2 < sizeof(wrong_at)) {
            strcat(wrong_at, what);
            strcat(wrong_at, " ");
        }
    }
}

bool clock_configure(enum clock_index clk, uint32_t src, uint32_t auxsrc, uint32_t src_freq, uint32_t freq) {
    (void)src, (void)auxsrc, (void)src_freq;
    if (clk == clk_sys) {
        step("clk_sys");
        clk_sys_switches++;
        if (!irq_off) clk_sys_irq_on++;
        clk_sys_hz = freq; // the state machines run from here on at freq / div
    } else if (clk == clk_peri) {
        step("clk_peri");
        clk_peri_sets++;
    }
    return true;
}
void pll_init(PLL pll, uint32_t refdiv, uint32_t vco_freq, uint32_t post_div1, uint32_t post_div2) {
    (void)pll, (void)refdiv, (void)vco_freq, (void)post_div1, (void)post_div2;
    step("pll_init");
    pll_inits++;
}
void pll_deinit(PLL pll) {
    (void)pll;
    step("pll_deinit");
    pll_deinits++;
}
uint32_t save_and_disable_interrupts(void) {
    step("irq_off");
    irq_off++;
    return 0;
}
void restore_interrupts(uint32_t status) {
    (void)status;
    irq_off--;
    step("irq_on"); // the first point an interrupt can run again
}

// ---- the tests ------------------------------------------------------------------

static void reset(void) {
    memset(host_pio, 0, sizeof(host_pio));
    host_pio[0].ctrl         = 1u << (PIO_CTRL_SM_ENABLE_LSB + 0);
    host_pio[0].sm[0].clkdiv = (FULL_HZ / (SM_HZ / 256u)) << 8; // 15.625 at 125 MHz
    steps = steps_wrong = clk_sys_irq_on = clk_sys_switches = pll_inits = pll_deinits = clk_peri_sets = 0;
    wrong_at[0]                                                                                       = 0;
}

static void test_switch(uint8_t idx, uint32_t low_hz) {
    global_saved_values.scan_deep_clock_idx = idx;
    reset();
    sval_clock_low();
    CHECK(sval_clock_is_low());
    CHECK(clk_sys_hz == low_hz);
    CHECK(clk_sys_switches == 1 && pll_deinits == 1 && clk_peri_sets == 1);
    CHECK(clk_sys_irq_on == 0); // nothing runs between the switch and the new dividers
    CHECK(irq_off == 0);
    CHECK(sm_rate_right());
    CHECK(steps_wrong == 0);
    if (steps_wrong) printf("  down to %u MHz, wrong rate at: %s\n", (unsigned)(low_hz / 1000000u), wrong_at);

    steps = steps_wrong = clk_sys_irq_on = clk_sys_switches = clk_peri_sets = 0;
    wrong_at[0]                                                             = 0;
    sval_clock_full();
    CHECK(!sval_clock_is_low());
    CHECK(clk_sys_hz == FULL_HZ);
    CHECK(clk_sys_switches == 1 && pll_inits == 1 && clk_peri_sets == 1);
    CHECK(clk_sys_irq_on == 0);
    CHECK(irq_off == 0);
    CHECK(sm_rate_right());
    CHECK(host_pio[0].sm[0].clkdiv == (FULL_HZ / (SM_HZ / 256u)) << 8); // the exact full-clock divider back
    CHECK(steps_wrong == 0);
    if (steps_wrong) printf("  back up from %u MHz, wrong rate at: %s\n", (unsigned)(low_hz / 1000000u), wrong_at);
}

int main(void) {
    test_switch(0, 48000000u);
    test_switch(1, 24000000u);
    test_switch(2, 12000000u);
    printf("clock switch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
