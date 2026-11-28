// SPDX-License-Identifier: GPL-2.0-or-later
// Host tests for the touchpad gesture guard (azoteq/pad_gesture_guard.h),
// with the read sequences from the ticket 0729 diagnostics (x4.6.6-clickdiag):
// the phantom right clicks are ignored, real taps still click.
#include <stdio.h>
#include <string.h>
#include "pad_gesture_guard.h"

static int fails, checks;
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static const pad_guard_cfg_t all_on = {.tap = true, .two_finger = true, .hold = true, .swipes = false, .tap_ms = 150};

static pad_guard_t g;
static uint32_t    now;

// One read: cycle time, gesture events 0 and 1, system info 0, fingers.
typedef struct {
    uint8_t why, ge0, ge1;
} out_t;
static out_t rd(const pad_guard_cfg_t *cfg, uint8_t cycle, uint8_t ge0, uint8_t ge1, uint8_t sys0, uint8_t fingers, bool woke) {
    uint8_t b[PAD_GUARD_BASE_BYTES] = {cycle, ge0, ge1, sys0, 0, fingers, 0, 0, 0, 0};
    out_t   o;
    o.why = pad_guard_frame(&g, cfg, b, now, woke);
    o.ge0 = b[PAD_B_GE0];
    o.ge1 = b[PAD_B_GE1];
    return o;
}

// A one-finger touch of ms, read every 10 ms, then the lift.
static void touch(uint8_t fingers, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10, now += 10)
        rd(&all_on, 10, 0, 0, 0, fingers, false);
}

static void reset(void) {
    pad_guard_init(&g);
    now = 100000;
}

// The four phantoms: a real one-finger touch, about 4-6 s later a lone read
// with two-finger tap and scroll and no fingers, about 1.4 s later another.
static void test_ticket_phantoms(void) {
    static const uint32_t gaps_ms[][2] = {{3820, 1400}, {4540, 1410}, {6330, 1460}, {3780, 1400}};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        touch(1, 300);
        rd(&all_on, 10, 0, 0, 0, 0, false); // the lift
        now += gaps_ms[i][0];
        // As the chip sent it, if the cycle time was real (it was not logged).
        out_t a = rd(&all_on, 50, 0, 0x03, 0x03, 0, false);
        CHECK(!(a.ge1 & PAD_GE1_TWO_TAP), "phantom %u: two-finger tap passed (ge1 %02X)", i, a.ge1);
        CHECK(a.why == PAD_GUARD_CONFLICT, "phantom %u: reason %u", i, a.why);
        now += gaps_ms[i][1];
        out_t b = rd(&all_on, 50, 0, 0x03, 0x03, 0, false);
        CHECK(!(b.ge1 & PAD_GE1_TWO_TAP) && b.why, "phantom %u second: passed", i);
    }
}

// The same reads, as a read one register late would give them in LP1: the
// cycle time byte holds gesture events 0 (0), gesture events 1 holds system
// info 0 (CHARGING_MODE 011). The whole frame is dropped.
static void test_shifted_frame(void) {
    reset();
    touch(1, 200);
    now += 4000;
    out_t a = rd(&all_on, 0, 0, 0x03, 0x00, 0, false);
    CHECK(a.why == PAD_GUARD_BAD_FRAME, "shifted frame: reason %u", a.why);
    // LP2 (100) would put zoom there; dropped all the same.
    a = rd(&all_on, 0, 0, 0x04, 0x00, 0, false);
    CHECK(a.why == PAD_GUARD_BAD_FRAME, "shifted LP2 frame: reason %u", a.why);
}

// A two-finger tap alone, with no two-finger frame before it.
static void test_lone_two_finger_tap(void) {
    reset();
    touch(1, 200);
    now += 5000;
    out_t a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(!(a.ge1 & PAD_GE1_TWO_TAP) && a.why == PAD_GUARD_NO_TWO, "lone tap: ge1 %02X reason %u", a.ge1, a.why);
    // Only one finger just before: still not a two-finger tap.
    touch(1, 80);
    a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(!(a.ge1 & PAD_GE1_TWO_TAP), "after one finger: ge1 %02X", a.ge1);
}

// A gesture in a read that had to wake the chip from low power.
static void test_woke(void) {
    reset();
    touch(2, 80);
    out_t a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, true);
    CHECK(!(a.ge1 & PAD_GE1_TWO_TAP) && a.why == PAD_GUARD_WOKE, "woke: ge1 %02X reason %u", a.ge1, a.why);
}

// Real gestures: still click.
static void test_real_two_finger_tap(void) {
    static const uint32_t down_ms[] = {20, 60, 100, 140};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        touch(1, 10); // the first finger lands a cycle early
        touch(2, down_ms[i]);
        out_t a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false); // reported after the lift
        CHECK(a.ge1 == PAD_GE1_TWO_TAP && a.why == PAD_GUARD_OK, "real tap %u: ge1 %02X reason %u", i, a.ge1, a.why);
    }
    // Reported a few reads late (missed reads): within the tap time + margin.
    reset();
    touch(2, 80);
    now += 200;
    out_t a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(a.ge1 == PAD_GE1_TWO_TAP, "late real tap: ge1 %02X", a.ge1);
    // A long tap time (1 s setting) widens the window.
    pad_guard_cfg_t slow = all_on;
    slow.tap_ms          = 1000;
    reset();
    touch(2, 900);
    now += 100;
    a = rd(&slow, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(a.ge1 == PAD_GE1_TWO_TAP, "slow real tap: ge1 %02X", a.ge1);
}

// The ticket's real one-finger taps: touch down, the tap 50-70 ms later.
static void test_real_taps(void) {
    static const uint32_t ms[] = {60, 50, 70};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        touch(1, ms[i]);
        out_t a = rd(&all_on, 10, PAD_GE0_TAP, 0, 0, 0, false);
        CHECK(a.ge0 == PAD_GE0_TAP && a.why == PAD_GUARD_OK, "real tap %u: ge0 %02X reason %u", i, a.ge0, a.why);
    }
    // Press and hold while the finger is down.
    reset();
    touch(1, 400);
    out_t a = rd(&all_on, 10, PAD_GE0_HOLD, 0, 0, 1, false);
    CHECK(a.ge0 == PAD_GE0_HOLD, "hold: ge0 %02X", a.ge0);
    // A tap long after any touch is not believed.
    now += 3000;
    a = rd(&all_on, 10, PAD_GE0_TAP, 0, 0, 0, false);
    CHECK(a.ge0 == 0 && a.why == PAD_GUARD_NO_TOUCH, "lone tap: ge0 %02X reason %u", a.ge0, a.why);
}

// Scroll and zoom are left alone (scroll moves while two fingers are down).
static void test_scroll(void) {
    reset();
    touch(2, 100);
    out_t a = rd(&all_on, 10, 0, PAD_GE1_SCROLL, 0, 2, false);
    CHECK(a.ge1 == PAD_GE1_SCROLL && a.why == PAD_GUARD_OK, "scroll: ge1 %02X reason %u", a.ge1, a.why);
}

// The Keybard settings: off never clicks, whatever the chip reports.
static void test_settings(void) {
    pad_guard_cfg_t c = all_on;
    c.two_finger      = false;
    reset();
    touch(2, 80);
    out_t a = rd(&c, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(a.ge1 == 0 && a.why == PAD_GUARD_OK, "two-finger off: ge1 %02X", a.ge1);
    c     = all_on;
    c.tap = false;
    reset();
    touch(1, 60);
    a = rd(&c, 10, PAD_GE0_TAP, 0, 0, 0, false);
    CHECK(a.ge0 == 0, "tap off: ge0 %02X", a.ge0);
    c      = all_on;
    c.hold = false;
    reset();
    touch(1, 400);
    a = rd(&c, 10, PAD_GE0_HOLD, 0, 0, 1, false);
    CHECK(a.ge0 == 0, "hold off: ge0 %02X", a.ge0);
}

// Swipes (buttons 3-6): off in every Svalboard build, so never; when a build
// turns them on, only with a touch just before.
static void test_swipes(void) {
    reset();
    touch(1, 100);
    out_t a = rd(&all_on, 10, 0x04, 0, 0, 1, false);
    CHECK(a.ge0 == 0, "swipe, swipes off: ge0 %02X", a.ge0);
    pad_guard_cfg_t c = all_on;
    c.swipes          = true;
    a                 = rd(&c, 10, 0x04, 0, 0, 1, false);
    CHECK(a.ge0 == 0x04, "swipe on, touching: ge0 %02X", a.ge0);
    now += 5000;
    a = rd(&c, 10, 0x08, 0, 0, 0, false);
    CHECK(a.ge0 == 0 && a.why == PAD_GUARD_NO_TOUCH, "swipe on, no touch: ge0 %02X", a.ge0);
}

// Bytes no real frame holds.
static void test_bad_frames(void) {
    reset();
    CHECK(rd(&all_on, 10, 0x40, 0, 0, 0, false).why == PAD_GUARD_BAD_FRAME, "ge0 unused bit");
    CHECK(rd(&all_on, 10, 0, 0x08, 0, 0, false).why == PAD_GUARD_BAD_FRAME, "ge1 unused bit");
    CHECK(rd(&all_on, 10, 0, 0, 0, 6, false).why == PAD_GUARD_BAD_FRAME, "6 fingers");
    CHECK(rd(&all_on, 10, 0, 0, 0, 0, false).why == PAD_GUARD_OK, "a quiet frame");
    // A bad frame's fingers do not count as a touch.
    rd(&all_on, 0, 0, 0, 0, 2, false);
    CHECK(rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false).ge1 == 0, "fingers of a bad frame counted");
}

// The clock wraps.
static void test_wrap(void) {
    pad_guard_init(&g);
    now = UINT32_MAX - 40;
    touch(2, 60);
    out_t a = rd(&all_on, 10, 0, PAD_GE1_TWO_TAP, 0, 0, false);
    CHECK(a.ge1 == PAD_GE1_TWO_TAP, "wrap: ge1 %02X", a.ge1);
}

int main(void) {
    test_ticket_phantoms();
    test_shifted_frame();
    test_lone_two_finger_tap();
    test_woke();
    test_real_two_finger_tap();
    test_real_taps();
    test_scroll();
    test_settings();
    test_swipes();
    test_bad_frames();
    test_wrap();
    printf("gesture guard: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
