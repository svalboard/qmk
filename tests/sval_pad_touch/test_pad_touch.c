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

static void test_size(void) {
    sval_pad_touch_t t = {0};
    uint8_t          s[SVAL_TOUCH_SIZE_BYTES];
    sval_pad_touch_pack_size(&t, s);
    CHECK(s[0] == 0 && s[1] == 0 && s[2] == 0, "never read: no size");
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 500, 500, 0);
    sval_pad_touch_size(&t, 1, 0x0312, 3);
    sval_pad_touch_pack_size(&t, s);
    CHECK(s[0] == 0x12 && s[1] == 0x03 && s[2] == 3, "strength little-endian, then area: %02X %02X %02X", s[0], s[1], s[2]);
    sval_pad_touch_sample(&t, &cfg, true, 0, 0, 0, 0, 0, 50);
    sval_pad_touch_size(&t, 0, 0x0312, 3);
    sval_pad_touch_pack_size(&t, s);
    CHECK(s[0] == 0 && s[2] == 0, "no finger: no size");
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

// A 45-byte read from 0x000C: fingers at pixel positions with an area each,
// and the gesture bytes.
typedef struct {
    uint8_t  slot;
    uint16_t x, y;
    uint8_t  area;
} slot_t;

static void raw_read(uint8_t *raw, uint8_t ge1, uint8_t info1, int n, const slot_t *s) {
    memset(raw, 0, 45);
    raw[2] = ge1;
    raw[4] = info1;
    raw[5] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *p = raw + 10 + 7 * s[i].slot;
        p[0]       = s[i].x >> 8;
        p[1]       = s[i].x & 0xFF;
        p[2]       = s[i].y >> 8;
        p[3]       = s[i].y & 0xFF;
        p[5]       = 50; // strength
        p[6]       = s[i].area;
    }
}

static uint32_t pos(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static void test_fingers(void) {
    sval_pad_fingers_t f = {0};
    uint8_t            raw[45], b[SVAL_FINGERS_BLOCK_BYTES];
    sval_pad_fingers_pack(&f, &cfg, 0, b);
    CHECK(b[0] == 0, "never read: not valid");
    raw_read(raw, 0, 0, 0, NULL);
    sval_pad_fingers_sample(&f, &cfg, false, raw, 0);
    sval_pad_fingers_pack(&f, &cfg, 0, b);
    CHECK(b[0] == 0, "a failed read changes nothing");

    // Three fingers: every slot's position (0.02 mm) and area.
    const slot_t three[3] = {{0, 1024, 896, 3}, {1, 512, 448, 4}, {3, 2047, 1791, 12}};
    raw_read(raw, 0, 0, 3, three);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 100);
    sval_pad_fingers_pack(&f, &cfg, 100, b);
    CHECK(b[0] == SVAL_FINGERS_VALID, "valid, no two-finger touch (%02X)", b[0]);
    CHECK(b[1] == 0x0B, "slots 0, 1 and 3 (%02X)", b[1]);
    CHECK((pos(b + 2) & 0xFFF) == 1075 && (pos(b + 2) >> 12) == 1000, "slot 0 at the middle, 0.02 mm: %u %u", pos(b + 2) & 0xFFF, pos(b + 2) >> 12);
    CHECK(b[5] == 3 && b[9] == 4 && b[17] == 12, "areas %u %u %u", b[5], b[9], b[17]);
    CHECK(pos(b + 10) == 0 && b[13] == 0, "an empty slot is zero");
    CHECK((pos(b + 14) & 0xFFF) == 2149 && (pos(b + 14) >> 12) == 1999, "slot 3 at the far corner: %u %u", pos(b + 14) & 0xFFF, pos(b + 14) >> 12);
    CHECK(pos(b + 18) == 0, "slot 4 empty");

    // The chip's palm and too-many flags (0x0011 bits 1, 2) pass through.
    raw_read(raw, 0, 0x06, 0, NULL);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 110);
    sval_pad_fingers_pack(&f, &cfg, 110, b);
    CHECK(b[0] == (SVAL_FINGERS_VALID | SVAL_FINGERS_PALM | SVAL_FINGERS_TOO_MANY) && b[1] == 0, "palm and too many (%02X)", b[0]);
}

static void test_two_fingers(void) {
    sval_pad_fingers_t f = {0};
    uint8_t            raw[45], b[SVAL_FINGERS_BLOCK_BYTES];
    // One finger, then a second lands: the two-finger touch starts there.
    const slot_t one[1] = {{0, 800, 900, 3}};
    raw_read(raw, 0, 0, 1, one);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1000);
    slot_t two[2] = {{0, 800, 900, 3}, {1, 1200, 900, 3}};
    raw_read(raw, 0, 0, 2, two);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1020);
    sval_pad_fingers_pack(&f, &cfg, 1020, b);
    CHECK(b[0] == (SVAL_FINGERS_VALID | SVAL_FINGERS_TWO | SVAL_FINGERS_HAD_TWO), "two fingers: live (%02X)", b[0]);
    // Centroid 1000, 900 px = 21.0, 20.09 mm = 1050, 1005 in 0.02 mm.
    CHECK((pos(b + 22) & 0xFFF) == 1050 && (pos(b + 22) >> 12) == 1005, "start centroid %u %u", pos(b + 22) & 0xFFF, pos(b + 22) >> 12);
    CHECK(b[25] == 0 && b[26] == 0, "no travel yet");
    // Both move 48 px down (1.07 mm): the average travel is 1.07 mm.
    two[0].y = two[1].y = 948;
    raw_read(raw, 0, 0, 2, two);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1100);
    sval_pad_fingers_pack(&f, &cfg, 1100, b);
    CHECK(b[25] == 21, "average travel 1.07 mm = 21 x 0.05 mm (%u)", b[25]);
    CHECK(b[26] == 20, "80 ms since the second landed (%u x 4)", b[26]);
    CHECK(!(b[0] & SVAL_FINGERS_SCROLL), "no scroll yet");
    // Only one moves on, 96 px from its start: the average is half the sum.
    two[1].y = 996;
    raw_read(raw, 0x02, 0, 2, two); // and the chip reports a scroll
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1120);
    sval_pad_fingers_pack(&f, &cfg, 1120, b);
    CHECK(b[25] == 32, "average of 1.07 and 2.14 mm: 1.61 mm = 32 (%u)", b[25]);
    CHECK(b[0] & SVAL_FINGERS_SCROLL, "the chip's scroll shows");
    // A third finger ends the two-finger touch; its numbers stay.
    const slot_t three[3] = {{0, 800, 948, 3}, {1, 1200, 996, 3}, {2, 1600, 400, 3}};
    raw_read(raw, 0, 0, 3, three);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1200);
    sval_pad_fingers_pack(&f, &cfg, 1200, b);
    CHECK(!(b[0] & SVAL_FINGERS_TWO) && (b[0] & SVAL_FINGERS_HAD_TWO) && b[25] == 32 && b[26] == 25, "a third finger ends it; the last numbers stay (%02X %u %u)", b[0], b[25], b[26]);
    // Back to two after a third: the chip needs a new touch first.
    raw_read(raw, 0, 0, 2, two);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1220);
    sval_pad_fingers_pack(&f, &cfg, 1220, b);
    CHECK(!(b[0] & SVAL_FINGERS_TWO), "three to two: no new two-finger touch");
    // All lift; two land again: a new one, scroll cleared.
    raw_read(raw, 0, 0, 0, NULL);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1300);
    raw_read(raw, 0, 0, 2, two);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1400);
    sval_pad_fingers_pack(&f, &cfg, 1400, b);
    CHECK((b[0] & SVAL_FINGERS_TWO) && !(b[0] & SVAL_FINGERS_SCROLL) && b[25] == 0, "a fresh two-finger touch (%02X)", b[0]);
    // Both lift together and the chip reports a two-finger tap: an event, cleared by the read.
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1480);
    raw_read(raw, 0x01, 0, 0, NULL);
    sval_pad_fingers_sample(&f, &cfg, true, raw, 1490);
    sval_pad_fingers_pack(&f, &cfg, 1490, b);
    CHECK((b[0] & SVAL_FINGERS_TWO_TAP) && !(b[0] & SVAL_FINGERS_TWO) && b[1] == 0, "two-finger tap at the lift (%02X)", b[0]);
    CHECK(b[26] == 20, "it lasted 80 ms (%u)", b[26]);
    sval_pad_fingers_pack(&f, &cfg, 1500, b);
    CHECK(!(b[0] & SVAL_FINGERS_TWO_TAP), "read once");
    // The merge keeps a tap not yet read.
    uint8_t held[SVAL_FINGERS_BLOCK_BYTES] = {SVAL_FINGERS_VALID | SVAL_FINGERS_TWO_TAP}, fresh[SVAL_FINGERS_BLOCK_BYTES] = {SVAL_FINGERS_VALID};
    sval_pad_fingers_merge(held, fresh);
    CHECK(held[0] == (SVAL_FINGERS_VALID | SVAL_FINGERS_TWO_TAP), "merge keeps the tap");
}

// The half without USB: the main loop packs and merges after each pad read;
// the split callback takes. Events from reads between two replies all go out,
// once.
static void test_snapshot_handoff(void) {
    sval_pad_touch_t t                            = {0};
    uint8_t          snap[SVAL_TOUCH_BLOCK_BYTES] = {0}, blk[SVAL_TOUCH_BLOCK_BYTES], out[SVAL_TOUCH_BLOCK_BYTES];
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, 1024, 896, 100);
    sval_pad_touch_pack(&t, &cfg, 100, blk);
    sval_pad_touch_merge(snap, blk);
    sval_pad_touch_sample(&t, &cfg, true, 0, 0x01, 0, 1024, 896, 160); // lift with a tap
    sval_pad_touch_pack(&t, &cfg, 160, blk);
    sval_pad_touch_merge(snap, blk);
    sval_pad_touch_take(snap, out);
    CHECK((out[1] & (SVAL_TOUCH_EV_DOWN | SVAL_TOUCH_EV_UP | SVAL_TOUCH_EV_TAP)) == (SVAL_TOUCH_EV_DOWN | SVAL_TOUCH_EV_UP | SVAL_TOUCH_EV_TAP), "both reads' events in one reply (%02X)", out[1]);
    sval_pad_touch_take(snap, out);
    CHECK(out[1] == 0 && (out[0] & SVAL_TOUCH_VALID), "taken once; the state stays");
    uint8_t fs[SVAL_FINGERS_BLOCK_BYTES] = {0}, fb[SVAL_FINGERS_BLOCK_BYTES] = {SVAL_FINGERS_VALID | SVAL_FINGERS_TWO_TAP}, fo[SVAL_FINGERS_BLOCK_BYTES];
    sval_pad_fingers_merge(fs, fb);
    fb[0] = SVAL_FINGERS_VALID;
    sval_pad_fingers_merge(fs, fb); // a later read without the tap keeps it
    sval_pad_fingers_take(fs, fo);
    CHECK(fo[0] & SVAL_FINGERS_TWO_TAP, "the two-finger tap survives to the reply");
    sval_pad_fingers_take(fs, fo);
    CHECK(!(fo[0] & SVAL_FINGERS_TWO_TAP), "and goes out once");
}

int main(void) {
    test_snapshot_handoff();
    test_fingers();
    test_two_fingers();
    test_idle();
    test_tap();
    test_drift();
    test_hold_and_gestures();
    test_size();
    test_merge();
    printf("pad touch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
