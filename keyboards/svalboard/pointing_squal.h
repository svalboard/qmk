// Pure helpers for POINTING_SENSOR (Sval 0x39): per-hand trackball sensor samples
// (SQUAL, raw data sum, raw max and min, shutter), kept free of QMK headers so
// tests/sval_pointing can build them on the host. Layout: docs/protocol.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SVAL_SQUAL_FORMAT      1
#define SVAL_SQUAL_REPLY_LEN   25 // bytes after [0xDF] [0x39]
#define SVAL_SQUAL_RING        256
#define SVAL_SQUAL_BIN_MS      50
#define SVAL_SQUAL_SAMPLE_LEN  9
#define SVAL_SQUAL_PER_REPLY   2
#define SVAL_SQUAL_HEAD_LEN    7 // format, flags, first seq (2), count, next seq (2)

enum sval_squal_op {
    SVAL_SQUAL_OP_SAMPLES = 0, // up to two bins at or after a cursor
    SVAL_SQUAL_OP_STATS   = 1, // min, max and mean over the last bins
};

// Reply byte 1 (both ops).
#define SVAL_SQUAL_F_CAPABLE   0x01 // a PMW3360/PMW3389 runs on that hand
#define SVAL_SQUAL_F_3389      0x02 // the sensor is a PMW3389
#define SVAL_SQUAL_F_LOST      0x04 // SAMPLES: the cursor was older than the ring; the reply starts at the oldest bin
#define SVAL_SQUAL_F_MORE      0x08 // SAMPLES: more bins wait after this reply
#define SVAL_SQUAL_F_RIGHT     0x10 // the hand is the right one
#define SVAL_SQUAL_F_OP_SHIFT  5    // bits 5-6: the op answered
#define SVAL_SQUAL_F_LIFTED    0x80 // the newest bin saw the ball lifted

// One burst read after the six motion bytes.
typedef struct {
    uint8_t  motion; // the Motion byte: bit 7 MOT, bit 3 Lift_stat
    uint8_t  squal;
    uint8_t  rawsum;
    uint8_t  rawmax;
    uint8_t  rawmin;
    uint16_t shutter;
} sval_squal_read_t;

// One finished bin, SVAL_SQUAL_BIN_MS of reads (9 bytes on the wire).
typedef struct {
    uint8_t  squal, squal_min, squal_max; // mean, lowest, highest SQUAL of the bin
    uint8_t  rawsum;                      // mean Raw_Data_Sum
    uint8_t  rawmax, rawmin;              // highest Maximum_Raw_data, lowest Minimum_Raw_data
    uint16_t shutter;                     // mean shutter
    uint8_t  moving;                      // bits 0-6: reads with motion (127 at most); bit 7: lifted at some read
} sval_squal_sample_t;

// The bin being filled.
typedef struct {
    uint16_t reads;
    uint32_t squal_sum, rawsum_sum, shutter_sum;
    uint8_t  squal_min, squal_max, rawmax, rawmin;
    uint8_t  moving;
    bool     lifted;
} sval_squal_acc_t;

typedef struct {
    sval_squal_sample_t bin[SVAL_SQUAL_RING];
    uint16_t            head;  // sequence number of the next bin
    uint16_t            count; // bins held, at most SVAL_SQUAL_RING
} sval_squal_ring_t;

static inline void sval_squal_acc_clear(sval_squal_acc_t *a) {
    memset(a, 0, sizeof(*a));
    a->squal_min = 0xFF;
    a->rawmin    = 0xFF;
}

static inline void sval_squal_acc_add(sval_squal_acc_t *a, const sval_squal_read_t *r) {
    if (a->reads == 0) sval_squal_acc_clear(a);
    if (a->reads < 0xFFFF) a->reads++;
    a->squal_sum += r->squal;
    a->rawsum_sum += r->rawsum;
    a->shutter_sum += r->shutter;
    if (r->squal < a->squal_min) a->squal_min = r->squal;
    if (r->squal > a->squal_max) a->squal_max = r->squal;
    if (r->rawmax > a->rawmax) a->rawmax = r->rawmax;
    if (r->rawmin < a->rawmin) a->rawmin = r->rawmin;
    if ((r->motion & 0x80) && a->moving < 127) a->moving++;
    if (r->motion & 0x08) a->lifted = true;
}

// Close the bin into a sample. False (nothing written) when it holds no reads.
static inline bool sval_squal_acc_finish(const sval_squal_acc_t *a, sval_squal_sample_t *s) {
    if (a->reads == 0) return false;
    s->squal     = (uint8_t)((a->squal_sum + a->reads / 2) / a->reads);
    s->squal_min = a->squal_min;
    s->squal_max = a->squal_max;
    s->rawsum    = (uint8_t)((a->rawsum_sum + a->reads / 2) / a->reads);
    s->rawmax    = a->rawmax;
    s->rawmin    = a->rawmin;
    s->shutter   = (uint16_t)((a->shutter_sum + a->reads / 2) / a->reads);
    s->moving    = (uint8_t)(a->moving | (a->lifted ? 0x80 : 0));
    return true;
}

static inline void sval_squal_ring_push(sval_squal_ring_t *g, const sval_squal_sample_t *s) {
    g->bin[g->head % SVAL_SQUAL_RING] = *s;
    g->head++;
    if (g->count < SVAL_SQUAL_RING) g->count++;
}

static inline void sval_squal_ring_clear(sval_squal_ring_t *g) {
    g->count = 0; // head keeps counting, so an old cursor reads as lost
}

static inline void sval_squal_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline uint8_t sval_squal_flags(uint8_t base, uint8_t op) {
    return (uint8_t)(base | (op << SVAL_SQUAL_F_OP_SHIFT));
}

// SAMPLES: the bins from `cursor` on, two at most. `out` is SVAL_SQUAL_REPLY_LEN bytes.
//   0 format | 1 flags | 2-3 seq of the first bin returned | 4 count | 5-6 next seq (head) | 7.. bins
static inline void sval_squal_pack_samples(uint8_t *out, const sval_squal_ring_t *g, uint16_t cursor, uint8_t flags) {
    memset(out, 0, SVAL_SQUAL_REPLY_LEN);
    uint16_t tail = (uint16_t)(g->head - g->count);
    uint16_t from = cursor;
    if ((uint16_t)(g->head - cursor) > g->count) { // older than the ring, or ahead of it (the count was cleared)
        from = tail;
        flags |= SVAL_SQUAL_F_LOST;
    }
    uint16_t avail = (uint16_t)(g->head - from);
    uint8_t  n     = avail > SVAL_SQUAL_PER_REPLY ? SVAL_SQUAL_PER_REPLY : (uint8_t)avail;
    if (avail > n) flags |= SVAL_SQUAL_F_MORE;
    out[0] = SVAL_SQUAL_FORMAT;
    out[1] = sval_squal_flags(flags, SVAL_SQUAL_OP_SAMPLES);
    sval_squal_put16(&out[2], from);
    out[4] = n;
    sval_squal_put16(&out[5], g->head);
    for (uint8_t i = 0; i < n; i++) {
        const sval_squal_sample_t *s = &g->bin[(uint16_t)(from + i) % SVAL_SQUAL_RING];
        uint8_t                   *p = &out[SVAL_SQUAL_HEAD_LEN + i * SVAL_SQUAL_SAMPLE_LEN];
        p[0]                         = s->squal;
        p[1]                         = s->squal_min;
        p[2]                         = s->squal_max;
        p[3]                         = s->rawsum;
        p[4]                         = s->rawmax;
        p[5]                         = s->rawmin;
        sval_squal_put16(&p[6], s->shutter);
        p[8] = s->moving;
    }
}

// STATS over the newest `window` bins (0 or more than held = all held):
//   0 format | 1 flags | 2-3 next seq | 4-5 bins in the window
//   6 SQUAL min | 7 SQUAL max | 8-9 SQUAL mean (x256)
//   10 raw sum min | 11 raw sum max | 12 raw sum mean
//   13 highest raw max | 14 lowest raw min
//   15-16 shutter min | 17-18 shutter max | 19-20 shutter mean
//   21 percent of bins with motion | 22-23 reads per second (newest bin) | 24 zero
static inline void sval_squal_pack_stats(uint8_t *out, const sval_squal_ring_t *g, uint16_t window, uint16_t reads_per_s, uint8_t flags) {
    memset(out, 0, SVAL_SQUAL_REPLY_LEN);
    uint16_t n = (window == 0 || window > g->count) ? g->count : window;
    out[0]     = SVAL_SQUAL_FORMAT;
    if (n && (g->bin[(uint16_t)(g->head - 1) % SVAL_SQUAL_RING].moving & 0x80)) flags |= SVAL_SQUAL_F_LIFTED;
    out[1] = sval_squal_flags(flags, SVAL_SQUAL_OP_STATS);
    sval_squal_put16(&out[2], g->head);
    sval_squal_put16(&out[4], n);
    if (n == 0) return;
    uint8_t  sq_min = 0xFF, sq_max = 0, rs_min = 0xFF, rs_max = 0, rmax = 0, rmin = 0xFF;
    uint16_t sh_min = 0xFFFF, sh_max = 0;
    uint32_t sq_sum = 0, rs_sum = 0, sh_sum = 0, moving = 0;
    for (uint16_t i = 0; i < n; i++) {
        const sval_squal_sample_t *s = &g->bin[(uint16_t)(g->head - 1 - i) % SVAL_SQUAL_RING];
        if (s->squal_min < sq_min) sq_min = s->squal_min;
        if (s->squal_max > sq_max) sq_max = s->squal_max;
        if (s->rawsum < rs_min) rs_min = s->rawsum;
        if (s->rawsum > rs_max) rs_max = s->rawsum;
        if (s->rawmax > rmax) rmax = s->rawmax;
        if (s->rawmin < rmin) rmin = s->rawmin;
        if (s->shutter < sh_min) sh_min = s->shutter;
        if (s->shutter > sh_max) sh_max = s->shutter;
        sq_sum += s->squal;
        rs_sum += s->rawsum;
        sh_sum += s->shutter;
        if (s->moving & 0x7F) moving++;
    }
    out[6] = sq_min;
    out[7] = sq_max;
    sval_squal_put16(&out[8], (uint16_t)((sq_sum * 256u) / n));
    out[10] = rs_min;
    out[11] = rs_max;
    out[12] = (uint8_t)(rs_sum / n);
    out[13] = rmax;
    out[14] = rmin;
    sval_squal_put16(&out[15], sh_min);
    sval_squal_put16(&out[17], sh_max);
    sval_squal_put16(&out[19], (uint16_t)(sh_sum / n));
    out[21] = (uint8_t)((moving * 100u) / n);
    sval_squal_put16(&out[22], reads_per_s);
}
