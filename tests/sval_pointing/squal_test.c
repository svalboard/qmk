// Host test for pointing_squal.h: the binning of PMW3360/PMW3389 quality reads and the
// POINTING_SENSOR (Sval 0x39) SAMPLES and STATS packing, cursor handling and ring wrap.
#include <stdio.h>
#include <string.h>
#include "pointing_squal.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static sval_squal_read_t rd(uint8_t motion, uint8_t sq, uint8_t sum, uint8_t mx, uint8_t mn, uint16_t sh) {
    sval_squal_read_t r = {motion, sq, sum, mx, mn, sh};
    return r;
}

static void test_bin(void) {
    sval_squal_acc_t    a;
    sval_squal_sample_t s;
    sval_squal_acc_clear(&a);
    CHECK(!sval_squal_acc_finish(&a, &s)); // no reads, no bin
    sval_squal_read_t r1 = rd(0x80, 10, 100, 60, 8, 400);
    sval_squal_read_t r2 = rd(0x00, 21, 102, 70, 5, 402);
    sval_squal_read_t r3 = rd(0x08, 30, 110, 65, 7, 410);
    sval_squal_acc_add(&a, &r1);
    sval_squal_acc_add(&a, &r2);
    sval_squal_acc_add(&a, &r3);
    CHECK(sval_squal_acc_finish(&a, &s));
    CHECK(s.squal == 20 && s.squal_min == 10 && s.squal_max == 30);
    CHECK(s.rawsum == 104); // (100+102+110)/3 = 104
    CHECK(s.rawmax == 70 && s.rawmin == 5);
    CHECK(s.shutter == 404); // (400+402+410)/3 = 404
    CHECK(s.moving == (1 | 0x80)); // one read with motion, the ball lifted at one read
}

static void fill(sval_squal_ring_t *g, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        sval_squal_sample_t s = {(uint8_t)i, (uint8_t)(i / 2), (uint8_t)(i + 5), (uint8_t)(100 + i % 50), (uint8_t)(i % 100 + 10), (uint8_t)(3 + i % 4), (uint16_t)(1000 + i), (uint8_t)(i % 3 ? 0 : 2)};
        sval_squal_ring_push(g, &s);
    }
}

static void test_samples(void) {
    static sval_squal_ring_t g;
    uint8_t                  out[SVAL_SQUAL_REPLY_LEN];
    memset(&g, 0, sizeof(g));
    sval_squal_pack_samples(out, &g, 0, SVAL_SQUAL_F_CAPABLE);
    CHECK(out[0] == 1 && out[4] == 0 && le16(&out[5]) == 0); // empty ring, cursor at head
    CHECK(!(out[1] & SVAL_SQUAL_F_LOST));
    CHECK(((out[1] >> SVAL_SQUAL_F_OP_SHIFT) & 3) == SVAL_SQUAL_OP_SAMPLES);

    fill(&g, 5);
    sval_squal_pack_samples(out, &g, 0, SVAL_SQUAL_F_CAPABLE | SVAL_SQUAL_F_RIGHT);
    CHECK(out[0] == 1 && (out[1] & SVAL_SQUAL_F_CAPABLE) && (out[1] & SVAL_SQUAL_F_RIGHT));
    CHECK(le16(&out[2]) == 0 && out[4] == 2 && le16(&out[5]) == 5);
    CHECK(out[1] & SVAL_SQUAL_F_MORE);
    CHECK(!(out[1] & SVAL_SQUAL_F_LOST));
    // bin 0 then bin 1, nine bytes each
    CHECK(out[7] == 0 && out[8] == 0 && out[9] == 5 && out[10] == 100 + 0 && out[11] == 10 && le16(&out[13]) == 1000 && out[15] == 2);
    CHECK(out[16] == 1 && le16(&out[22]) == 1001 && out[24] == 0);

    sval_squal_pack_samples(out, &g, 4, 0); // the last bin, nothing more after it
    CHECK(le16(&out[2]) == 4 && out[4] == 1 && !(out[1] & SVAL_SQUAL_F_MORE));
    CHECK(out[7] == 4 && le16(&out[13]) == 1004);
    CHECK(out[16] == 0 && out[24] == 0); // the second slot stays zero

    sval_squal_pack_samples(out, &g, 5, 0); // caught up
    CHECK(out[4] == 0 && le16(&out[2]) == 5 && le16(&out[5]) == 5);
}

static void test_wrap_and_lost(void) {
    static sval_squal_ring_t g;
    uint8_t                  out[SVAL_SQUAL_REPLY_LEN];
    memset(&g, 0, sizeof(g));
    fill(&g, SVAL_SQUAL_RING + 44); // bins 44..299 held, head 300
    CHECK(g.head == 300 && g.count == SVAL_SQUAL_RING);
    sval_squal_pack_samples(out, &g, 10, 0); // older than the ring
    CHECK(out[1] & SVAL_SQUAL_F_LOST);
    CHECK(le16(&out[2]) == 44 && out[4] == 2 && out[7] == (uint8_t)44);
    sval_squal_pack_samples(out, &g, 298, 0);
    CHECK(!(out[1] & SVAL_SQUAL_F_LOST) && out[4] == 2 && out[7] == (uint8_t)298 && out[16] == (uint8_t)299);
    sval_squal_pack_samples(out, &g, 400, 0); // ahead of the ring: the host restarted the count
    CHECK(out[1] & SVAL_SQUAL_F_LOST && le16(&out[2]) == 44);

    sval_squal_ring_clear(&g); // a new host session: the old cursor reads as lost, no stale bins
    sval_squal_pack_samples(out, &g, 298, 0);
    CHECK((out[1] & SVAL_SQUAL_F_LOST) && out[4] == 0 && le16(&out[5]) == 300);

    // sequence numbers wrap at 16 bits
    memset(&g, 0, sizeof(g));
    g.head = 65535;
    fill(&g, 3);
    CHECK(g.head == 2);
    sval_squal_pack_samples(out, &g, 65535, 0);
    CHECK(!(out[1] & SVAL_SQUAL_F_LOST) && le16(&out[2]) == 65535 && out[4] == 2 && out[1] & SVAL_SQUAL_F_MORE);
}

static void test_stats(void) {
    static sval_squal_ring_t g;
    uint8_t                  out[SVAL_SQUAL_REPLY_LEN];
    memset(&g, 0, sizeof(g));
    sval_squal_pack_stats(out, &g, 0, 0, SVAL_SQUAL_F_CAPABLE);
    CHECK(out[0] == 1 && le16(&out[4]) == 0 && out[6] == 0); // nothing yet
    CHECK(((out[1] >> SVAL_SQUAL_F_OP_SHIFT) & 3) == SVAL_SQUAL_OP_STATS);

    sval_squal_sample_t a = {40, 30, 50, 100, 120, 20, 500, 0};
    sval_squal_sample_t b = {60, 55, 70, 110, 90, 10, 700, 3};
    sval_squal_sample_t c = {80, 75, 90, 120, 100, 15, 600, 0x80 | 1};
    sval_squal_ring_push(&g, &a);
    sval_squal_ring_push(&g, &b);
    sval_squal_ring_push(&g, &c);
    sval_squal_pack_stats(out, &g, 0, 987, SVAL_SQUAL_F_CAPABLE);
    CHECK(le16(&out[2]) == 3 && le16(&out[4]) == 3);
    CHECK(out[1] & SVAL_SQUAL_F_LIFTED); // the newest bin saw it
    CHECK(out[6] == 30 && out[7] == 90 && le16(&out[8]) == 60 * 256);
    CHECK(out[10] == 100 && out[11] == 120 && out[12] == 110);
    CHECK(out[13] == 120 && out[14] == 10);
    CHECK(le16(&out[15]) == 500 && le16(&out[17]) == 700 && le16(&out[19]) == 600);
    CHECK(out[21] == 66); // two of three bins moved
    CHECK(le16(&out[22]) == 987 && out[24] == 0);

    sval_squal_pack_stats(out, &g, 1, 0, 0); // the newest bin alone
    CHECK(le16(&out[4]) == 1 && out[6] == 75 && out[7] == 90 && out[10] == 120 && le16(&out[15]) == 600);
    sval_squal_pack_stats(out, &g, 99, 0, 0); // more than held means all held
    CHECK(le16(&out[4]) == 3);

    memset(&g, 0, sizeof(g));
    fill(&g, 1000); // a full ring, mean in x256
    sval_squal_pack_stats(out, &g, 0, 0, 0);
    CHECK(le16(&out[4]) == SVAL_SQUAL_RING);
}

static void test_layout(void) {
    CHECK(SVAL_SQUAL_HEAD_LEN + SVAL_SQUAL_PER_REPLY * SVAL_SQUAL_SAMPLE_LEN == SVAL_SQUAL_REPLY_LEN);
    CHECK(sizeof(sval_squal_read_t) >= 7);
}

int main(void) {
    test_layout();
    test_bin();
    test_samples();
    test_wrap_and_lost();
    test_stats();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
