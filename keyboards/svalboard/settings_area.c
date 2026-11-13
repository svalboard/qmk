// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The settings area's record store (settings_area.h). Plain C over the store
// callbacks, so the host tests run the same file.

#include <string.h>
#include "settings_area.h"

static const settings_area_store_t *st;

void settings_area_use(const settings_area_store_t *store) {
    st = store;
}

static const uint8_t MAGIC[4] = {'S', 'V', 'S', 'A'};

static uint16_t rd16(uint16_t off) {
    uint8_t b[2];
    st->read(off, b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static void wr16(uint16_t off, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    st->write(off, b, 2);
}

static uint8_t crc8_step(uint8_t crc, uint8_t byte) { // CRC-8, polynomial 0x07
    crc ^= byte;
    for (uint8_t i = 0; i < 8; i++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    return crc;
}

static uint8_t seq_next(uint8_t s) {
    return s == 255 ? 1 : (uint8_t)(s + 1); // 0 marks a slot being written
}

#define RECORD_CAP_(name, id, cap) \
    case id:                       \
        return cap;
uint16_t settings_area_capacity(uint16_t id) {
    switch (id) {
        SVAL_SETTINGS_RECORDS(RECORD_CAP_)
        default:
            return 0;
    }
}
#undef RECORD_CAP_

bool settings_area_valid(void) {
    if (!st || st->size < SVAL_SETTINGS_HEADER_BYTES + 2) return false;
    uint8_t h[SVAL_SETTINGS_HEADER_BYTES];
    st->read(0, h, sizeof(h));
    return memcmp(h, MAGIC, 4) == 0 && h[4] == SVAL_SETTINGS_FORMAT && (uint16_t)(h[6] | (h[7] << 8)) == st->size;
}

void settings_area_format(void) {
    wr16(SVAL_SETTINGS_HEADER_BYTES, 0); // empty list first, then the header that makes it valid
    uint8_t h[SVAL_SETTINGS_HEADER_BYTES] = {'S', 'V', 'S', 'A', SVAL_SETTINGS_FORMAT, 0, (uint8_t)st->size, (uint8_t)(st->size >> 8)};
    st->write(0, h, sizeof(h));
}

// A slot's offset, sequence number (0: no valid value) and length.
typedef struct {
    uint16_t off;
    uint8_t  seq;
    uint16_t len;
} slot_t;

static slot_t slot_read(uint16_t entry, uint16_t id, uint16_t cap, uint8_t i) {
    slot_t s = {.off = (uint16_t)(entry + SVAL_SETTINGS_ENTRY_HEAD_BYTES + i * (SVAL_SETTINGS_SLOT_HEAD_BYTES + cap)), .seq = 0, .len = 0};
    uint8_t head[3];
    st->read(s.off, head, 3);
    uint16_t len = (uint16_t)(head[1] | (head[2] << 8));
    if (head[0] == 0 || len > cap) return s;
    uint8_t crc = crc8_step(crc8_step(0, (uint8_t)id), (uint8_t)(id >> 8));
    for (uint8_t k = 0; k < 3; k++) crc = crc8_step(crc, head[k]);
    uint8_t  buf[32];
    uint16_t done = 0;
    while (done < len) {
        uint16_t n = (uint16_t)(len - done) > (uint16_t)sizeof(buf) ? (uint16_t)sizeof(buf) : (uint16_t)(len - done);
        st->read(s.off + 3 + done, buf, n);
        for (uint16_t k = 0; k < n; k++) crc = crc8_step(crc, buf[k]);
        done += n;
    }
    uint8_t stored;
    st->read(s.off + 3 + len, &stored, 1);
    if (stored != crc) return s;
    s.seq = head[0];
    s.len = len;
    return s;
}

// The slot holding the current value, or -1.
static int8_t slot_current(const slot_t s[2]) {
    if (s[0].seq && s[1].seq) return s[1].seq == seq_next(s[0].seq) ? 1 : 0;
    if (s[0].seq) return 0;
    if (s[1].seq) return 1;
    return -1;
}

// Walk the list: the offset of id's entry (and its capacity), or 0. *end gets
// the offset of the end mark.
static uint16_t find(uint16_t id, uint16_t *cap_out, uint16_t *end) {
    uint16_t off = SVAL_SETTINGS_HEADER_BYTES;
    uint16_t hit = 0;
    while (off + 2 <= st->size) {
        uint16_t eid = rd16(off);
        if (eid == 0) break;
        if (off + SVAL_SETTINGS_ENTRY_HEAD_BYTES > st->size) break;
        uint16_t cap  = rd16(off + 2);
        uint32_t next = (uint32_t)off + SVAL_SETTINGS_ENTRY_BYTES((uint32_t)cap);
        if (next + 2 > st->size) break; // damaged: treat as the end
        if (eid == id) {
            hit      = off;
            *cap_out = cap;
        }
        off = (uint16_t)next;
    }
    *end = off;
    return hit;
}

bool settings_area_get(uint16_t id, void *buf, uint16_t max, uint16_t *len) {
    if (!settings_area_valid() || id == 0 || id == SVAL_SETTINGS_RETIRED_ID) return false;
    uint16_t cap = 0, end;
    uint16_t e = find(id, &cap, &end);
    if (!e) return false;
    slot_t s[2] = {slot_read(e, id, cap, 0), slot_read(e, id, cap, 1)};
    int8_t c    = slot_current(s);
    if (c < 0) return false;
    uint16_t n = s[c].len < max ? s[c].len : max;
    st->read(s[c].off + 3, buf, n);
    if (len) *len = s[c].len;
    return true;
}

static bool same(uint16_t off, const uint8_t *data, uint16_t len) {
    uint8_t  buf[32];
    uint16_t done = 0;
    while (done < len) {
        uint16_t n = (uint16_t)(len - done) > (uint16_t)sizeof(buf) ? (uint16_t)sizeof(buf) : (uint16_t)(len - done);
        st->read(off + done, buf, n);
        if (memcmp(buf, data + done, n)) return false;
        done += n;
    }
    return true;
}

bool settings_area_put(uint16_t id, const void *buf, uint16_t len) {
    if (!st || id == 0 || id == SVAL_SETTINGS_RETIRED_ID) return false;
    if (!settings_area_valid()) settings_area_format();
    const uint8_t *data = buf;
    uint16_t       cap = 0, end;
    uint16_t       e = find(id, &cap, &end);
    if (e && cap < len) { // too small for this firmware's value: retire it, start a new entry
        wr16(e, SVAL_SETTINGS_RETIRED_ID);
        e = 0;
    }
    if (!e) {
        uint16_t want = settings_area_capacity(id);
        if (want < len) want = len;
        uint32_t need = (uint32_t)end + SVAL_SETTINGS_ENTRY_BYTES((uint32_t)want) + 2;
        if (need > st->size) return false;
        // Everything but the id first: until the id is written, the list still ends here.
        wr16(end + 2, want);
        const uint8_t zero = 0;
        st->write(end + SVAL_SETTINGS_ENTRY_HEAD_BYTES, &zero, 1);
        st->write(end + SVAL_SETTINGS_ENTRY_HEAD_BYTES + SVAL_SETTINGS_SLOT_HEAD_BYTES + want, &zero, 1);
        wr16(end + SVAL_SETTINGS_ENTRY_BYTES(want), 0);
        wr16(end, id);
        e   = end;
        cap = want;
    }
    slot_t s[2] = {slot_read(e, id, cap, 0), slot_read(e, id, cap, 1)};
    int8_t c    = slot_current(s);
    if (c >= 0 && s[c].len == len && same(s[c].off + 3, data, len)) return true;
    uint8_t t   = c < 0 ? 0 : (uint8_t)(1 - c);
    uint8_t seq = c < 0 ? 1 : seq_next(s[c].seq);
    // Mark the slot as being written, fill it, then give it its sequence
    // number: a cut before that last byte leaves the slot invalid.
    const uint8_t zero = 0;
    st->write(s[t].off, &zero, 1);
    uint8_t head[3] = {seq, (uint8_t)len, (uint8_t)(len >> 8)};
    st->write(s[t].off + 1, head + 1, 2);
    st->write(s[t].off + 3, data, len);
    uint8_t crc = crc8_step(crc8_step(0, (uint8_t)id), (uint8_t)(id >> 8));
    for (uint8_t k = 0; k < 3; k++) crc = crc8_step(crc, head[k]);
    for (uint16_t k = 0; k < len; k++) crc = crc8_step(crc, data[k]);
    st->write(s[t].off + 3 + len, &crc, 1);
    st->write(s[t].off, &seq, 1);
    return true;
}
