// Host tests for keyboards/svalboard/pad_touch.c. Run by util/updater_test/run.sh.
//
// One pad's touch for the touch page of Sval 0x2D: touch down and up, the
// chip's gesture events, leaving the dead zone while a tap or a hold could
// still fire (and only then), the 16-byte block in 0.01 mm, the events a
// read clears and a merge keeps.
#include "pad_touch.h"
#include "pad_gestures.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            failures++;                                 \
        }                                               \
    } while (0)

// The native touchpad's view: 2048 x 1792, a 0.8 mm dead zone (38 px), tap
// 150 ms, press and hold on at 300 ms.
static const sval_pad_touch_cfg_t cfg = {.xres = 2048, .yres = 1792, .tap_px = 38, .tap_ms = 150, .hold_ms = 300, .hold_on = true};

static uint16_t u16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}

static void test_idle(void) {
    sval_pad_touch_t t = {0};
    uint8_t          b[SVAL_TOUCH_BLOCK_BYTES];
    sval_pad_touch_pack(&t, &cfg, 0, b);
    CHECK(b[0] == 0 && b[1] == 0, "never read: not valid");
    sval_pad_touch_sample(&t, &cfg, false, 1, 0, 0, 100, 100, 0);
    sval_pad_touch_pack(&t, &cfg, 0, b);
    CHECK(b[0] == 0, "a failed read changes nothing");
    sval_pad_touch_sample(&t, &cfg, true, 0, 0, 0, 0, 0, 10);
    sval_pad_touch_pack(&t, &cfg, 10, b);
    CHECK(b[0] == SVAL_TOUCH_VALID && b[1] == 0, "read, no finger: valid, no events (%02X %02X)", b[0], b[1]);
}

static void test_tap(void) {
    sval_pad_touch_t t = {0};
    uint8_t          b[SVAL_TOUCH_BLOCK_BYTES];
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 1024, 896, 1000);
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 1040, 900, 1050); // 16 px: inside
    sval_pad_touch_pack(&t, &cfg, 1060, b);
    CHECK(b[0] & SVAL_TOUCH_DOWN, "down");
    CHECK(((b[0] & SVAL_TOUCH_FINGERS) >> 4) == 1, "one finger");
    CHECK(!(b[0] & SVAL_TOUCH_OUTSIDE), "inside the dead zone");
    CHECK(b[1] == SVAL_TOUCH_EV_DOWN, "event: down only (%02X)", b[1]);
    CHECK(u16(b + 6) == 2150 && u16(b + 8) == 2000, "down point in 0.01 mm: %u %u", u16(b + 6), u16(b + 8));
    CHECK(u16(b + 2) == 2184, "x now 0.01 mm: %u", u16(b + 2));
    CHECK(u16(b + 10) == 60, "60 ms since down: %u", u16(b + 10));
    CHECK(u16(b + 12) >= 33 && u16(b + 12) <= 36, "farthest about 0.34 mm: %u", u16(b + 12));
    CHECK(u16(b + 14) == 38, "tap distance in force: %u", u16(b + 14));
    // Lift: the chip reports the tap on the release cycle with no fingers.
    sval_pad_touch_sample(&t, &cfg, true, 0, 0x01, 0, 1040, 900, 1100);
    sval_pad_touch_sample(&t, &cfg, true, 0, 0, 0, 0, 0, 1110);
    sval_pad_touch_pack(&t, &cfg, 1200, b);
    CHECK(!(b[0] & SVAL_TOUCH_DOWN), "up");
    CHECK(b[1] == (SVAL_TOUCH_EV_UP | SVAL_TOUCH_EV_TAP), "events: up and tap (%02X)", b[1]);
    CHECK(u16(b + 10) == 100, "the touch lasted 100 ms: %u", u16(b + 10));
    CHECK(u16(b + 2) == 2184, "the last position stays");
    sval_pad_touch_pack(&t, &cfg, 1300, b);
    CHECK(b[1] == 0, "a read clears the events");
}

static void test_drift(void) {
    sval_pad_touch_t t = {0};
    uint8_t          b[SVAL_TOUCH_BLOCK_BYTES];
    // Out at 100 ms: a tap could still fire, so the drift cancels it.
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 527, 527, 100); // 38.2 px diagonal: out
    sval_pad_touch_pack(&t, &cfg, 100, b);
    CHECK(b[0] & SVAL_TOUCH_OUTSIDE, "outside");
    CHECK(b[1] & SVAL_TOUCH_EV_DRIFT, "drift while the tap could fire");
    // Further out: no second drift event.
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 700, 500, 120);
    sval_pad_touch_pack(&t, &cfg, 120, b);
    CHECK(!(b[1] & SVAL_TOUCH_EV_DRIFT), "one drift per touch");
    CHECK(u16(b + 12) >= 420, "farthest grows: %u", u16(b + 12));

    // Exactly on the edge is inside (the chip's "more than").
    sval_pad_touch_t e = {0};
    sval_pad_touch_sample(&e, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&e, &cfg, true, 1, 0, 0, 538, 500, 10);
    sval_pad_touch_pack(&e, &cfg, 10, b);
    CHECK(!(b[0] & SVAL_TOUCH_OUTSIDE), "38 px is inside");

    // Out at 300 ms: the tap window is over but the hold (at 450) could still fire.
    sval_pad_touch_t h = {0};
    sval_pad_touch_sample(&h, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&h, &cfg, true, 1, 0, 0, 600, 500, 300);
    sval_pad_touch_pack(&h, &cfg, 300, b);
    CHECK(b[1] & SVAL_TOUCH_EV_DRIFT, "drift while the hold could fire");

    // Hold off: past the tap window, leaving cancels nothing.
    sval_pad_touch_cfg_t nohold = cfg;
    nohold.hold_on              = false;
    sval_pad_touch_t n          = {0};
    sval_pad_touch_sample(&n, &nohold, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&n, &nohold, true, 1, 0, 0, 600, 500, 300);
    sval_pad_touch_pack(&n, &nohold, 300, b);
    CHECK((b[0] & SVAL_TOUCH_OUTSIDE) && !(b[1] & SVAL_TOUCH_EV_DRIFT), "hold off: moving after the tap window is just moving");

    // After the hold window: moving is a pointer move, not a cancel.
    sval_pad_touch_t m = {0};
    sval_pad_touch_sample(&m, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&m, &cfg, true, 1, 0, 0, 600, 500, 500);
    sval_pad_touch_pack(&m, &cfg, 500, b);
    CHECK(!(b[1] & SVAL_TOUCH_EV_DRIFT), "after 450 ms: no drift");
}

static void test_hold_and_gestures(void) {
    sval_pad_touch_t t = {0};
    uint8_t          b[SVAL_TOUCH_BLOCK_BYTES];
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&t, &cfg, true, 1, 0x02, 0, 501, 500, 450);
    sval_pad_touch_sample(&t, &cfg, true, 1, 0x02, 0, 520, 500, 460);
    sval_pad_touch_pack(&t, &cfg, 460, b);
    CHECK(b[0] & SVAL_TOUCH_HOLDING, "holding");
    CHECK((b[1] & SVAL_TOUCH_EV_HOLD) && (b[1] & SVAL_TOUCH_EV_DOWN), "hold began once (%02X)", b[1]);
    // A drag past the dead zone while holding cancels nothing.
    sval_pad_touch_sample(&t, &cfg, true, 1, 0x02, 0, 800, 500, 470);
    sval_pad_touch_pack(&t, &cfg, 470, b);
    CHECK(!(b[1] & (SVAL_TOUCH_EV_DRIFT | SVAL_TOUCH_EV_HOLD)), "dragging: no drift, no second hold (%02X)", b[1]);
    sval_pad_touch_sample(&t, &cfg, true, 0, 0, 0, 0, 0, 900);
    sval_pad_touch_pack(&t, &cfg, 900, b);
    CHECK(!(b[0] & SVAL_TOUCH_HOLDING) && (b[1] & SVAL_TOUCH_EV_UP), "lift ends the hold");

    sval_pad_touch_t g = {0};
    sval_pad_touch_sample(&g, &cfg, true, 2, 0, 0, 500, 500, 0);
    sval_pad_touch_sample(&g, &cfg, true, 0, 0, 0x01, 500, 500, 80);
    sval_pad_touch_sample(&g, &cfg, true, 2, 0, 0x02, 500, 500, 200);
    sval_pad_touch_sample(&g, &cfg, true, 1, 0x04, 0x04, 500, 500, 300);
    sval_pad_touch_pack(&g, &cfg, 300, b);
    CHECK(b[1] & SVAL_TOUCH_EV_TWO_TAP, "two-finger tap");
    CHECK(b[1] & SVAL_TOUCH_EV_SCROLL, "scroll");
    CHECK(b[1] & SVAL_TOUCH_EV_OTHER, "swipe or zoom");
    CHECK(((b[0] & SVAL_TOUCH_FINGERS) >> 4) == 1, "one finger now");
    sval_pad_touch_sample(&g, &cfg, true, 6, 0, 0, 0, 0, 310);
    sval_pad_touch_pack(&g, &cfg, 310, b);
    CHECK(((b[0] & SVAL_TOUCH_FINGERS) >> 4) == 1, "an impossible finger count is ignored");
}

static void test_merge(void) {
    uint8_t held[SVAL_TOUCH_BLOCK_BYTES] = {0}, fresh[SVAL_TOUCH_BLOCK_BYTES] = {0};
    held[0]  = SVAL_TOUCH_VALID | SVAL_TOUCH_DOWN;
    held[1]  = SVAL_TOUCH_EV_DOWN | SVAL_TOUCH_EV_DRIFT;
    fresh[0] = SVAL_TOUCH_VALID;
    fresh[1] = SVAL_TOUCH_EV_UP;
    fresh[2] = 7;
    sval_pad_touch_merge(held, fresh);
    CHECK(held[0] == SVAL_TOUCH_VALID && held[2] == 7, "a merge takes the fresh state");
    CHECK(held[1] == (SVAL_TOUCH_EV_DOWN | SVAL_TOUCH_EV_DRIFT | SVAL_TOUCH_EV_UP), "and keeps the events not yet read");
}

int main(void) {
    test_idle();
    test_tap();
    test_drift();
    test_hold_and_gestures();
    test_merge();
    printf("pad touch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
