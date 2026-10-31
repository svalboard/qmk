// Host tests for keyboards/svalboard/pad_gestures.c. Run by util/updater_test/run.sh.
//
// The touchpad tap setting: 0 is the build's defaults (tap, two-finger tap
// and press and hold on, hold 300 ms), each switch on its own, the hold time snapped to
// its table, the stored record (never written, damaged, or left by other
// firmware reads as the defaults), the chip's gesture register bytes, and the
// split sync with older halves.
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

// A board that never changed the setting runs as every build did before it.
static void test_defaults(void) {
    CHECK(sval_pad_tap_on(0), "tap on by default");
    CHECK(sval_pad_two_finger_on(0), "two-finger tap on by default");
    CHECK(sval_pad_hold_on(0), "press and hold on by default");
    CHECK(sval_pad_hold_time_ms(0) == 300, "hold 300 ms by default, got %u", sval_pad_hold_time_ms(0));
}

static void test_switches(void) {
    const uint8_t bits[3] = {SVAL_PAD_TAP_OFF, SVAL_PAD_TWO_FINGER_OFF, SVAL_PAD_HOLD_OFF};
    for (int i = 0; i < 3; i++) {
        uint8_t s = sval_pad_with(0, bits[i], false);
        CHECK(sval_pad_tap_on(s) == (i != 0), "switch %d off: tap", i);
        CHECK(sval_pad_two_finger_on(s) == (i != 1), "switch %d off: two-finger", i);
        CHECK(sval_pad_hold_on(s) == (i != 2), "switch %d off: hold", i);
        CHECK(sval_pad_hold_time_ms(s) == 300, "switch %d off keeps the hold time", i);
        CHECK(sval_pad_with(s, bits[i], true) == 0, "switch %d back on is the defaults again", i);
        CHECK(sval_pad_with(s, bits[i], false) == s, "switch %d off twice is the same", i);
    }
    uint8_t all = sval_pad_with(sval_pad_with(sval_pad_with(0, SVAL_PAD_TAP_OFF, false), SVAL_PAD_TWO_FINGER_OFF, false), SVAL_PAD_HOLD_OFF, false);
    CHECK(!sval_pad_tap_on(all) && !sval_pad_two_finger_on(all) && !sval_pad_hold_on(all), "all off");
    // Only the three switch bits move.
    CHECK(sval_pad_with(0, 0xFF, false) == (SVAL_PAD_TAP_OFF | SVAL_PAD_TWO_FINGER_OFF | SVAL_PAD_HOLD_OFF), "other bits ignored");
}

static void test_hold_time(void) {
    for (int i = 0; i < SVAL_PAD_HOLD_CHOICES; i++) {
        uint8_t s = sval_pad_with_hold_ms(0, sval_pad_hold_ms[i]);
        CHECK(sval_pad_hold_time_ms(s) == sval_pad_hold_ms[i], "exact %u", sval_pad_hold_ms[i]);
        CHECK((s & ~SVAL_PAD_KNOWN) == 0, "bit 7 never set");
        if (i > 1) CHECK(sval_pad_hold_ms[i] > sval_pad_hold_ms[i - 1], "the table rises after the default");
        for (int j = 0; j < i; j++)
            CHECK(sval_pad_hold_ms[i] != sval_pad_hold_ms[j], "no time twice");
    }
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 0)) == 20, "0 snaps to 20");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 34)) == 20, "34 snaps to 20");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 35)) == 20, "a tie takes the shorter");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 275)) == 250, "275: a tie with the default takes the shorter");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 290)) == 300, "290 snaps to the default");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 325)) == 300, "325: a tie above the default takes it");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 36)) == 50, "36 snaps to 50");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 299)) == 300, "299 snaps to 300");
    CHECK(sval_pad_hold_time_ms(sval_pad_with_hold_ms(0, 65535)) == 1500, "max snaps to 1500");
    // The default is index 0: choosing 300 is the default byte, and 20, the
    // old default, is a change like any other.
    CHECK(sval_pad_with_hold_ms(0, 300) == 0, "300 ms is the default byte");
    CHECK(sval_pad_with_hold_ms(0, 20) != 0, "20 ms is stored as a change");
    // The hold time keeps the switches, and the switches keep the hold time.
    uint8_t s = sval_pad_with(0, SVAL_PAD_TAP_OFF, false);
    s         = sval_pad_with_hold_ms(s, 500);
    CHECK(!sval_pad_tap_on(s) && sval_pad_hold_time_ms(s) == 500, "hold time keeps the switches");
    s = sval_pad_with(s, SVAL_PAD_HOLD_OFF, false);
    CHECK(sval_pad_hold_time_ms(s) == 500, "a switch keeps the hold time");
}

static void test_record(void) {
    uint8_t rec[SVAL_PAD_RECORD_BYTES];
    for (int s = 0; s <= SVAL_PAD_KNOWN; s++) {
        sval_pad_record_write((uint8_t)s, rec);
        CHECK(sval_pad_record_read(rec) == s, "record round trip %d", s);
    }
    // Never written: erased (0xFF) or zeroed flash, or bytes another firmware left.
    const uint8_t erased[2] = {0xFF, 0xFF}, zero[2] = {0, 0}, junk[2] = {0x12, 0x34}, high[2] = {0x80, 0x7F}, one[2] = {0x05, 0x00};
    CHECK(sval_pad_record_read(erased) == 0, "erased reads as the defaults");
    CHECK(sval_pad_record_read(zero) == 0, "zeroed reads as the defaults");
    CHECK(sval_pad_record_read(junk) == 0, "junk reads as the defaults");
    CHECK(sval_pad_record_read(high) == 0, "bit 7 reads as the defaults");
    CHECK(sval_pad_record_read(one) == 0, "a half-written record reads as the defaults");
}

// The 8 bytes from 0x06B7: single-finger enables, multi-finger enables, tap
// time, tap distance, hold time, all 16-bit big-endian.
static void test_regs(void) {
    // Example bytes after the driver's start on a Svalboard build: tap and
    // press and hold on, two-finger tap and scroll on, hold time 300 ms.
    const uint8_t start[SVAL_PAD_REG_BYTES] = {0x03, 0x03, 0x00, 0x96, 0x00, 0x19, 0x01, 0x2C};
    uint8_t       regs[SVAL_PAD_REG_BYTES];

    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(0, regs);
    CHECK(memcmp(regs, start, sizeof(regs)) == 0, "the defaults write what the start wrote");

    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with(0, SVAL_PAD_TAP_OFF, false), regs);
    CHECK(regs[0] == 0x02 && regs[1] == 0x03, "tap off: %02X %02X", regs[0], regs[1]);

    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with(0, SVAL_PAD_HOLD_OFF, false), regs);
    CHECK(regs[0] == 0x01 && regs[1] == 0x03, "hold off: %02X %02X", regs[0], regs[1]);

    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with(0, SVAL_PAD_TWO_FINGER_OFF, false), regs);
    CHECK(regs[0] == 0x03 && regs[1] == 0x02, "two-finger off keeps scroll: %02X %02X", regs[0], regs[1]);

    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with_hold_ms(0, 20), regs);
    CHECK(regs[6] == 0x00 && regs[7] == 0x14, "hold 20 ms: %02X %02X", regs[6], regs[7]);
    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with_hold_ms(0, 1000), regs);
    CHECK(regs[6] == 0x03 && regs[7] == 0xE8, "hold 1000 ms big-endian: %02X %02X", regs[6], regs[7]);
    CHECK(memcmp(regs, start, 6) == 0, "hold time leaves the rest");

    // Swipes (bits 2-5) and zoom (multi bit 2) as read are kept.
    uint8_t other[SVAL_PAD_REG_BYTES] = {0x3C, 0x06, 1, 2, 3, 4, 5, 6};
    sval_pad_regs_apply(0x07, other);
    CHECK(other[0] == 0x3C && other[1] == 0x06, "other gestures kept: %02X %02X", other[0], other[1]);
    CHECK(other[2] == 1 && other[3] == 2 && other[4] == 3 && other[5] == 4, "times and distances kept");
}

static void test_sync(void) {
    for (int s = 0; s <= SVAL_PAD_KNOWN; s += 5) {
        uint8_t b = sval_pad_sync_byte((uint8_t)s);
        CHECK(b & SVAL_PAD_SYNC_VALID, "sync byte marked");
        CHECK(sval_pad_sync_setting(true, b, 0x33) == s, "the other half takes %d", s);
    }
    // The defaults travel too: a change back to them reaches the other half.
    CHECK(sval_pad_sync_setting(true, sval_pad_sync_byte(0), 0x07) == 0, "back to the defaults");
    // An older half with USB: a shorter ping, or a zero byte, keeps the other half's own.
    CHECK(sval_pad_sync_setting(false, 0, 0x07) == 0x07, "no byte: keeps its own");
    CHECK(sval_pad_sync_setting(true, 0x00, 0x07) == 0x07, "unmarked byte: keeps its own");
    CHECK(sval_pad_sync_setting(false, 0x85, 0x07) == 0x07, "not present: ignores the byte");
}

int main(void) {
    test_defaults();
    test_switches();
    test_hold_time();
    test_record();
    test_regs();
    test_sync();
    printf("pad gestures: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
