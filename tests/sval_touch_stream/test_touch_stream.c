// Host tests for the touch page of POINTING_MOTION (Sval 0x2D page 1,
// keyboards/svalboard/azoteq/touch_stream.c). Run by util/updater_test/run.sh.
//
// The page's first finger is the first contact slot with a touch strength.
// A contact keeps its slot until it lifts, so with two fingers down and the
// first one lifted, slot 0 is empty and the finger left is in slot 1.
#include "../../keyboards/svalboard/azoteq/touch_stream.c"

#include <assert.h>
#include <stdio.h>

static uint32_t now_ms = 1000;

uint32_t timer_read32(void) {
    return now_ms;
}
uint32_t timer_elapsed32(uint32_t last) {
    return now_ms - last;
}
bool is_keyboard_master(void) {
    return true;
}
bool is_keyboard_left(void) {
    return true;
}
bool transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    (void)id;
    (void)in_len;
    (void)in;
    (void)out_len;
    (void)out;
    return false;
}
bool is_transport_connected(void) {
    return false;
}
uint8_t sval_pad_gestures(void) {
    return 0;
}
uint16_t sval_pad_tuning(void) {
    return 0;
}
uint16_t sval_pad_scroll_in_force_mm100(void) {
    return 0;
}
uint8_t sval_pad_palm(void) {
    return 0;
}
uint8_t sval_pad_hands(void) {
    return 1; // this (left) hand only
}
bool sval_mt_os_touchpad(uint8_t pad) {
    (void)pad;
    return false;
}
void sval_pad_touch_config(sval_pad_touch_cfg_t *cfg) {
    *cfg = (sval_pad_touch_cfg_t){.xres = 2048, .yres = 1792, .tap_px = 25, .tap_ms = 150, .hold_ms = 300, .hold_on = true};
}

typedef struct {
    uint16_t x, y, strength;
    uint8_t  area;
} slot_t;

// The 45 bytes from register 0x000C: fingers at byte 5, slot i at 10 + 7 i
// (x, y, strength big-endian, then the area). A zero strength: no finger.
static void read_of(uint8_t raw[45], uint8_t fingers, const slot_t s[5]) {
    memset(raw, 0, 45);
    raw[5] = fingers;
    for (int i = 0; i < 5; i++) {
        uint8_t *p = raw + 10 + 7 * i;
        p[0]       = s[i].x >> 8;
        p[1]       = s[i].x & 0xFF;
        p[2]       = s[i].y >> 8;
        p[3]       = s[i].y & 0xFF;
        p[4]       = s[i].strength >> 8;
        p[5]       = s[i].strength & 0xFF;
        p[6]       = s[i].area;
    }
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

// The page's left pad: position (0.01 mm) and the first finger's size.
static void page(uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *area) {
    uint8_t out[SVAL_TOUCH_PAGE_BYTES] = {0};
    sval_pointing_touch_kb(out, sizeof(out));
    assert(out[0] == SVAL_TOUCH_PAGE_FORMAT && (out[14] & SVAL_TOUCH_VALID));
    *x        = le16(&out[14 + 2]);
    *y        = le16(&out[14 + 4]);
    *strength = le16(&out[48]);
    *area     = out[50];
}

// Where a finger at x, y (pixels) shows on the page, from pad_touch.c alone.
static void expected(uint16_t px, uint16_t py, uint16_t *x, uint16_t *y) {
    sval_pad_touch_t     t = {0};
    sval_pad_touch_cfg_t cfg;
    sval_pad_touch_config(&cfg);
    sval_pad_touch_sample(&t, &cfg, true, 1, 0, 0, px, py, now_ms);
    uint8_t blk[SVAL_TOUCH_BLOCK_BYTES];
    sval_pad_touch_pack(&t, &cfg, now_ms, blk);
    *x = le16(&blk[2]);
    *y = le16(&blk[4]);
}

static void test_first_finger_lifts(void) {
    uint16_t x, y, s, ex, ey;
    uint8_t  a, raw[45];
    uint8_t  first[SVAL_TOUCH_PAGE_BYTES];
    sval_pointing_touch_kb(first, sizeof(first)); // the host reads the page: the pad is followed from here
    // One finger, slot 0.
    read_of(raw, 1, (slot_t[5]){{400, 300, 120, 3}});
    sval_touch_sample(raw, true);
    now_ms += 8;
    page(&x, &y, &s, &a);
    expected(400, 300, &ex, &ey);
    assert(x == ex && y == ey && s == 120 && a == 3);
    // A second finger lands in slot 1.
    read_of(raw, 2, (slot_t[5]){{400, 300, 120, 3}, {1500, 1200, 200, 5}});
    sval_touch_sample(raw, true);
    now_ms += 8;
    page(&x, &y, &s, &a);
    assert(x == ex && y == ey && s == 120 && a == 3);
    // The first finger lifts: slot 0 is empty, the finger left is in slot 1.
    read_of(raw, 1, (slot_t[5]){{0, 0, 0, 0}, {1510, 1205, 210, 6}});
    sval_touch_sample(raw, true);
    now_ms += 8;
    page(&x, &y, &s, &a);
    expected(1510, 1205, &ex, &ey);
    assert(x == ex && y == ey);
    assert(s == 210 && a == 6);
    // It moves: the page follows it.
    read_of(raw, 1, (slot_t[5]){{0, 0, 0, 0}, {1600, 1250, 220, 6}});
    sval_touch_sample(raw, true);
    now_ms += 8;
    page(&x, &y, &s, &a);
    expected(1600, 1250, &ex, &ey);
    assert(x == ex && y == ey && s == 220);
    // All up: no size.
    read_of(raw, 0, (slot_t[5]){{0}});
    sval_touch_sample(raw, true);
    now_ms += 8;
    page(&x, &y, &s, &a);
    assert(s == 0 && a == 0);
    puts("  the first finger lifts: the page follows the finger left in slot 1");
}

int main(void) {
    puts("touch page:");
    test_first_finger_lifts();
    puts("touch page: all passed");
    return 0;
}
