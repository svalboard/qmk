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

// The touchpad tuning: 0 is a 1.2 mm dead zone, a 150 ms tap time and the
// build's own scroll distance.
static void test_tune_defaults(void) {
    CHECK(sval_pad_dead_zone(0) == 120, "dead zone 1.2 mm by default, got %u", sval_pad_dead_zone(0));
    CHECK(sval_pad_tap_time_ms(0) == 150, "tap time 150 ms by default (the driver's 0x96)");
    CHECK(sval_pad_scroll_start(0) == 0, "scroll distance: the build's own by default");
    CHECK(sval_pad_tune_with_dead_zone(0, 120) == 0, "1.2 mm is the default word");
    CHECK(sval_pad_tune_with_dead_zone(0, 80) != 0, "0.8 mm is a choice, not the default");
    CHECK(sval_pad_tune_with_tap_ms(0, 150) == 0, "150 ms is the default word");
    CHECK(sval_pad_tune_with_scroll(0, 0) == 0, "scroll 0 is the build's own");
}

static void test_tune_tables(void) {
    const uint16_t *tables[3] = {sval_pad_dead_zone_mm100, sval_pad_tap_ms, sval_pad_scroll_mm100};
    for (int k = 0; k < 3; k++)
        for (int i = 2; i < SVAL_PAD_TUNE_CHOICES; i++)
            CHECK(tables[k][i] > tables[k][i - 1], "table %d rises after the default at %d", k, i);
    for (int i = 0; i < SVAL_PAD_TUNE_CHOICES; i++) {
        uint16_t t = sval_pad_tune_with_dead_zone(0, sval_pad_dead_zone_mm100[i]);
        CHECK(sval_pad_dead_zone(t) == sval_pad_dead_zone_mm100[i], "dead zone exact %u", sval_pad_dead_zone_mm100[i]);
        t = sval_pad_tune_with_tap_ms(0, sval_pad_tap_ms[i]);
        CHECK(sval_pad_tap_time_ms(t) == sval_pad_tap_ms[i], "tap time exact %u", sval_pad_tap_ms[i]);
        t = sval_pad_tune_with_scroll(0, sval_pad_scroll_mm100[i]);
        CHECK(sval_pad_scroll_start(t) == sval_pad_scroll_mm100[i], "scroll exact %u", sval_pad_scroll_mm100[i]);
        CHECK((t & ~SVAL_PAD_TUNE_KNOWN) == 0, "bits 12-15 never set");
    }
    CHECK(sval_pad_dead_zone(sval_pad_tune_with_dead_zone(0, 0)) == 20, "0 snaps to 0.2 mm");
    CHECK(sval_pad_dead_zone(sval_pad_tune_with_dead_zone(0, 85)) == 80, "a tie takes the smaller");
    CHECK(sval_pad_dead_zone(sval_pad_tune_with_dead_zone(0, 52)) == 50, "0.52 mm (today's release) snaps to 0.5");
    CHECK(sval_pad_dead_zone(sval_pad_tune_with_dead_zone(0, 65535)) == 400, "max snaps to 4 mm");
    CHECK(sval_pad_tap_time_ms(sval_pad_tune_with_tap_ms(0, 0)) == 50, "0 ms snaps to 50");
    CHECK(sval_pad_scroll_start(sval_pad_tune_with_scroll(0, 1)) == 30, "1 snaps to 0.3 mm, not the build's own");
    // Each part keeps the others.
    uint16_t t = sval_pad_tune_with_dead_zone(0, 120);
    t          = sval_pad_tune_with_tap_ms(t, 250);
    t          = sval_pad_tune_with_scroll(t, 200);
    CHECK(sval_pad_dead_zone(t) == 120 && sval_pad_tap_time_ms(t) == 250 && sval_pad_scroll_start(t) == 200, "three parts kept");
    t = sval_pad_tune_with_tap_ms(t, 150);
    CHECK(sval_pad_dead_zone(t) == 120 && sval_pad_scroll_start(t) == 200, "back to the default tap time keeps the rest");
}

// The same millimeters in every build: pixels from the resolution in force.
static void test_tune_pixels(void) {
    // The native touchpad: 2048 across 43 mm. The 1.2 mm default is 57
    // pixels, 0.8 mm 38; the driver's 25 pixels were 0.52 mm.
    CHECK(sval_pad_mm100_to_px(120, 2048, SVAL_PAD_WIDTH_MM100) == 57, "1.2 mm at full resolution: %u", sval_pad_mm100_to_px(120, 2048, SVAL_PAD_WIDTH_MM100));
    CHECK(sval_pad_mm100_to_px(80, 2048, SVAL_PAD_WIDTH_MM100) == 38, "0.8 mm at full resolution: %u", sval_pad_mm100_to_px(80, 2048, SVAL_PAD_WIDTH_MM100));
    CHECK(sval_pad_px_to_mm100(25, 2048, SVAL_PAD_WIDTH_MM100) == 52, "25 px at full resolution: %u", sval_pad_px_to_mm100(25, 2048, SVAL_PAD_WIDTH_MM100));
    // Without it, at 800 cpi: 800 * 43 / 25.4 = 1354 pixels. 1.2 mm is 38
    // pixels there; 0.8 mm is 25, which is what those builds always had.
    CHECK(sval_pad_mm100_to_px(120, 1354, SVAL_PAD_WIDTH_MM100) == 38, "1.2 mm at 800 cpi: %u", sval_pad_mm100_to_px(120, 1354, SVAL_PAD_WIDTH_MM100));
    CHECK(sval_pad_mm100_to_px(80, 1354, SVAL_PAD_WIDTH_MM100) == 25, "0.8 mm at 800 cpi: %u", sval_pad_mm100_to_px(80, 1354, SVAL_PAD_WIDTH_MM100));
    // At 400 cpi (677 pixels) it is 13: still 0.8 mm.
    CHECK(sval_pad_mm100_to_px(80, 677, SVAL_PAD_WIDTH_MM100) == 13, "0.8 mm at 400 cpi");
    CHECK(sval_pad_mm100_to_px(1, 100, SVAL_PAD_WIDTH_MM100) == 1, "never 0 pixels");
    CHECK(sval_pad_mm100_to_px(400, 4096, SVAL_PAD_WIDTH_MM100) == 381, "4 mm at the largest resolution");
    CHECK(sval_pad_px_to_mm100(50, 2048, SVAL_PAD_WIDTH_MM100) == 105, "the build's own scroll distance at full resolution is 1.05 mm");
}

static void test_tune_regs(void) {
    // 19 bytes from 0x06B7, as the driver's start leaves them on a Svalboard
    // build: tap time 150, tap distance 25, hold 300, swipes, scroll 50.
    uint8_t start[SVAL_PAD_TUNE_REG_BYTES] = {0x03, 0x03, 0x00, 0x96, 0x00, 0x19, 0x01, 0x2C, 0x00, 0x64, 0x01, 0x2C, 0x00, 0x00, 0x07, 0xD0, 0x17, 0x00, 0x32};
    uint8_t regs[SVAL_PAD_TUNE_REG_BYTES];

    memcpy(regs, start, sizeof(regs));
    sval_pad_tune_regs_apply(0, 2048, 0x32, regs);
    CHECK(regs[4] == 0 && regs[5] == 57, "the defaults at full resolution: 57 px, got %u", regs[5]);
    CHECK(regs[2] == 0x00 && regs[3] == 0x96, "the default tap time is the start's");
    CHECK(regs[17] == 0 && regs[18] == 0x32, "the build's own scroll distance");
    regs[5] = 0x19;
    CHECK(memcmp(regs, start, sizeof(regs)) == 0, "only the tap distance differs from the start");

    memcpy(regs, start, sizeof(regs));
    sval_pad_tune_regs_apply(0, 1354, 0x32, regs);
    CHECK(regs[4] == 0 && regs[5] == 38, "the defaults at 800 cpi: 38 px, got %u", regs[5]);
    regs[5] = 0x19;
    CHECK(memcmp(regs, start, sizeof(regs)) == 0, "at 800 cpi too, only the tap distance differs from the start");

    memcpy(regs, start, sizeof(regs));
    sval_pad_tune_regs_apply(sval_pad_tune_with_dead_zone(0, 80), 1354, 0x32, regs);
    CHECK(memcmp(regs, start, sizeof(regs)) == 0, "0.8 mm at 800 cpi writes what those builds always had");

    memcpy(regs, start, sizeof(regs));
    uint16_t t = sval_pad_tune_with_tap_ms(sval_pad_tune_with_dead_zone(sval_pad_tune_with_scroll(0, 200), 300), 500);
    sval_pad_tune_regs_apply(t, 2048, 0x32, regs);
    CHECK(regs[2] == 0x01 && regs[3] == 0xF4, "tap time 500 big-endian");
    CHECK(regs[4] == 0 && regs[5] == 143, "3 mm is 143 px, got %u", regs[5]);
    CHECK(regs[17] == 0 && regs[18] == 95, "scroll 2 mm is 95 px, got %u", regs[18]);
    CHECK(memcmp(regs, start, 2) == 0 && memcmp(regs + 6, start + 6, 11) == 0, "enables, hold and swipes kept");

    // The tap setting's 8 bytes and the tuning's 19 work on one buffer.
    memcpy(regs, start, sizeof(regs));
    sval_pad_regs_apply(sval_pad_with_hold_ms(0, 500), regs);
    sval_pad_tune_regs_apply(0, 2048, 0x32, regs);
    CHECK(regs[6] == 0x01 && regs[7] == 0xF4 && regs[5] == 57, "hold and dead zone together");
}

static void test_tune_record_sync(void) {
    uint8_t rec[SVAL_PAD_TUNE_RECORD_BYTES];
    for (uint32_t t = 0; t <= SVAL_PAD_TUNE_KNOWN; t += 37) {
        sval_pad_tune_record_write((uint16_t)t, rec);
        CHECK(sval_pad_tune_record_read(rec) == t, "tuning record round trip %u", (unsigned)t);
    }
    const uint8_t erased[4] = {0xFF, 0xFF, 0xFF, 0xFF}, zero[4] = {0}, half[4] = {0x12, 0x03, 0xED, 0x00}, high[4] = {0x00, 0x10, 0xFF, 0xEF};
    CHECK(sval_pad_tune_record_read(erased) == 0, "erased reads as the defaults");
    CHECK(sval_pad_tune_record_read(zero) == 0, "zeroed reads as the defaults");
    CHECK(sval_pad_tune_record_read(half) == 0, "a half-written record reads as the defaults");
    CHECK(sval_pad_tune_record_read(high) == 0, "bits 12-15 read as the defaults");

    for (uint32_t t = 0; t <= SVAL_PAD_TUNE_KNOWN; t += 101) {
        uint16_t w = sval_pad_tune_sync_word((uint16_t)t);
        CHECK(w & SVAL_PAD_TUNE_SYNC_VALID, "sync word marked");
        CHECK(sval_pad_tune_sync_setting(true, w, 0x123) == t, "the other half takes %u", (unsigned)t);
    }
    CHECK(sval_pad_tune_sync_setting(false, 0, 0x123) == 0x123, "an older half with USB: keeps its own");
    CHECK(sval_pad_tune_sync_setting(true, 0, 0x123) == 0x123, "an unmarked word: keeps its own");
    CHECK(sval_pad_tune_sync_setting(true, sval_pad_tune_sync_word(0), 0x123) == 0, "back to the defaults travels");
}

static void test_palm(void) {
    uint8_t rec[SVAL_PAD_PALM_RECORD_BYTES] = {0, 0};
    CHECK(sval_pad_palm_record_read(rec) == 0, "never written: off");
    rec[0] = 0xFF;
    rec[1] = 0xFF;
    CHECK(sval_pad_palm_record_read(rec) == 0, "erased flash: off");
    sval_pad_palm_record_write(12, rec);
    CHECK(rec[0] == 12 && rec[1] == (uint8_t)~12, "the byte and its complement");
    CHECK(sval_pad_palm_record_read(rec) == 12, "reads back");
    rec[0] = 200;
    rec[1] = (uint8_t)~200;
    CHECK(sval_pad_palm_record_read(rec) == 0, "above the pad's channels: off");
    CHECK(sval_pad_palm_clamp(300) == SVAL_PAD_PALM_MAX && sval_pad_palm_clamp(7) == 7, "a write is clamped to the channels");
    CHECK(!sval_pad_is_palm(0, 50), "off: nothing is a palm");
    CHECK(!sval_pad_is_palm(10, 10) && sval_pad_is_palm(10, 11), "above the threshold is a palm");
    CHECK(sval_pad_palm_sync_byte(9) == (SVAL_PAD_PALM_SYNC_VALID | 9), "sync byte marked");
    CHECK(sval_pad_palm_sync_setting(false, 0, 9) == 9, "an older half with USB: keeps its own");
    CHECK(sval_pad_palm_sync_setting(true, 5, 9) == 9, "an unmarked byte: keeps its own");
    CHECK(sval_pad_palm_sync_setting(true, sval_pad_palm_sync_byte(0), 9) == 0, "off travels");
    CHECK(sval_pad_palm_sync_setting(true, sval_pad_palm_sync_byte(20), 0) == 20, "a threshold travels");
}

static void test_mode(void) {
    uint8_t rec[SVAL_PAD_MODE_RECORD_BYTES] = {0, 0};
    CHECK(sval_pad_mode_record_read(rec) == SVAL_PAD_MODE_AUTO, "never written: Automatic");
    rec[0] = rec[1] = 0xFF;
    CHECK(sval_pad_mode_record_read(rec) == SVAL_PAD_MODE_AUTO, "erased flash: Automatic");
    sval_pad_mode_record_write(SVAL_PAD_MODE_BUILTIN, rec);
    CHECK(rec[0] == 1 && rec[1] == 0xFE, "the byte and its complement");
    CHECK(sval_pad_mode_record_read(rec) == SVAL_PAD_MODE_BUILTIN, "reads back");
    rec[0] = 7;
    rec[1] = (uint8_t)~7;
    CHECK(sval_pad_mode_record_read(rec) == SVAL_PAD_MODE_AUTO, "an unknown mode: Automatic");
    sval_pad_mode_record_write(9, rec);
    CHECK(rec[0] == SVAL_PAD_MODE_AUTO, "an unknown write stores Automatic");
    CHECK(sval_pad_mode_sync_byte(SVAL_PAD_MODE_BUILTIN) == 0x81, "sync byte marked");
    CHECK(sval_pad_mode_sync_setting(false, 0, SVAL_PAD_MODE_BUILTIN) == SVAL_PAD_MODE_BUILTIN, "an older half with USB: keeps its own");
    CHECK(sval_pad_mode_sync_setting(true, 1, SVAL_PAD_MODE_AUTO) == SVAL_PAD_MODE_AUTO, "an unmarked byte: keeps its own");
    CHECK(sval_pad_mode_sync_setting(true, 0x81, SVAL_PAD_MODE_AUTO) == SVAL_PAD_MODE_BUILTIN, "built-in travels");
    CHECK(sval_pad_mode_sync_setting(true, 0x80, SVAL_PAD_MODE_BUILTIN) == SVAL_PAD_MODE_AUTO, "Automatic travels");
}

int main(void) {
    test_mode();
    test_palm();
    test_defaults();
    test_switches();
    test_hold_time();
    test_record();
    test_regs();
    test_sync();
    test_tune_defaults();
    test_tune_tables();
    test_tune_pixels();
    test_tune_regs();
    test_tune_record_sync();
    printf("pad gestures: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
