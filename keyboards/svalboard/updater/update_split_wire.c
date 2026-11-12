// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Split-link wire format for the updater (see update_split_wire.h).

#include <string.h>
#include "update_split_wire.h"

// ---- checksums ------------------------------------------------------------------

uint8_t usplit_crc8(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++) crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
    }
    return crc;
}

uint16_t usplit_crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (uint8_t b = 0; b < 8; b++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = (v >> 16) & 0xFF;
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get24(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

// ---- build identity ------------------------------------------------------------

uint32_t usplit_git_hash32(const char *s, bool *dirty) {
    uint32_t v = 0;
    size_t   i = 0;
    if (dirty) *dirty = false;
    if (!s) return 0;
    for (; i < 8; i++) {
        char    c = s[i];
        uint8_t d;
        if (c >= '0' && c <= '9') {
            d = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint8_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint8_t)(c - 'A' + 10);
        } else {
            return 0; // "NA" (SKIP_GIT), or too short to name a commit
        }
        v = (v << 4) | d;
    }
    if (dirty) {
        while (s[i]) {
            if (s[i] == '*') *dirty = true;
            i++;
        }
    }
    return v;
}

// ---- presence ------------------------------------------------------------------

void usplit_presence_encode(update_presence_t *p, const update_build_id_t *self, bool updating) {
    memset(p, 0, sizeof(*p));
    p->magic         = UPDATE_PRESENCE_MAGIC;
    p->struct_ver    = UPDATE_PRESENCE_STRUCT_VERSION;
    p->fw_version    = self->fw_version;
    p->git_hash      = self->git_hash;
    p->updater_proto = self->updater_proto;
    p->hand          = self->hand;
    p->pointing_id   = self->pointing_id;
    p->flags         = (self->git_dirty ? UPDATE_PRESENCE_FLAG_DIRTY : 0) | (updating ? UPDATE_PRESENCE_FLAG_UPDATING : 0);
    p->crc8          = usplit_crc8((const uint8_t *)p, sizeof(*p) - 1);
}

uint8_t usplit_presence_compare(const void *rsp, size_t len, const update_build_id_t *self, update_presence_t *out) {
    update_presence_t p;
    if (len < sizeof(p)) return UPDATE_PRESENCE_INVALID;
    memcpy(&p, rsp, sizeof(p));
    if (out) *out = p;
    if (p.magic != UPDATE_PRESENCE_MAGIC || p.struct_ver != UPDATE_PRESENCE_STRUCT_VERSION) return UPDATE_PRESENCE_INVALID;
    if (p.crc8 != usplit_crc8((const uint8_t *)&p, sizeof(p) - 1)) return UPDATE_PRESENCE_INVALID;
    if (p.hand == self->hand) return UPDATE_PRESENCE_SAME_HAND;
    if (p.fw_version != self->fw_version || p.git_hash != self->git_hash || p.updater_proto != self->updater_proto) return UPDATE_PRESENCE_VERSION;
    return UPDATE_PRESENCE_MATCH;
}

// ---- KEYBOARD_UPDATE frames ----------------------------------------------------

int usplit_rsp_payload_len(uint8_t op) {
    switch (op) {
        case USPLIT_OP_INFO:
            return USPLIT_INFO_PAYLOAD;
        case USPLIT_OP_BEGIN:
            return 1;
        case USPLIT_OP_PAGE:
            return 3;
        case USPLIT_OP_STATUS:
            return USPLIT_STATUS_PAYLOAD;
        case USPLIT_OP_PAGE2:
            return 3;
        case USPLIT_OP_END:
        case USPLIT_OP_COMMIT:
        case USPLIT_OP_ABORT:
        case USPLIT_OP_SETTINGS_RESET:
        case USPLIT_OP_FAST:
            return 0;
#ifdef SVAL_UPDATE_TEST_HOOKS
        case USPLIT_OP_TEST_RESET:
            return 0;
#endif
        default:
            return -1;
    }
}

uint8_t usplit_rsp_len(uint8_t op) {
    int n = usplit_rsp_payload_len(op);
    return n < 0 ? 0 : (uint8_t)(USPLIT_RSP_HDR + n + USPLIT_CRC_BYTES);
}

_Static_assert(USPLIT_RSP_HDR + USPLIT_INFO_PAYLOAD + USPLIT_CRC_BYTES <= USPLIT_MSG_MAX, "INFO response fits the RPC buffer");
_Static_assert(USPLIT_RSP_HDR + USPLIT_STATUS_PAYLOAD + USPLIT_CRC_BYTES <= USPLIT_MSG_MAX, "STATUS response fits the RPC buffer");

// The op's payload rules; fills the decoded fields of r. Shared by build and parse.
static bool payload_ok(uint8_t op, const uint8_t *pl, uint8_t len, usplit_req_t *r) {
    switch (op) {
        case USPLIT_OP_INFO:
        case USPLIT_OP_STATUS:
        case USPLIT_OP_END:
        case USPLIT_OP_ABORT:
            return len == 0;
        case USPLIT_OP_BEGIN:
            if (len < 2) return false;
            r->off  = pl[0];
            r->n    = pl[1];
            r->data = pl + 2;
            return r->n >= 1 && r->n <= USPLIT_BEGIN_DATA_MAX && len == 2 + r->n && r->off + r->n <= UPDATE_MANIFEST_BYTES;
        case USPLIT_OP_PAGE:
            if (len < 4) return false;
            r->off  = get24(pl);
            r->n    = pl[3];
            r->data = pl + 4;
            return r->n >= 1 && r->n <= USPLIT_PAGE_DATA_MAX && len == 4 + r->n && (r->off % USPLIT_PAGE_BYTES) + r->n <= USPLIT_PAGE_BYTES;
        case USPLIT_OP_PAGE2:
            if (len < 4) return false;
            r->off  = get24(pl);
            r->n    = pl[3];
            r->data = pl + 4;
            return r->n >= 1 && r->n <= USPLIT_PAGE2_DATA_MAX && len == 4 + r->n;
        case USPLIT_OP_FAST:
            if (len != 1) return false;
            r->mode = pl[0];
            return r->mode == 1;
#ifdef SVAL_UPDATE_TEST_HOOKS
        case USPLIT_OP_TEST_RESET:
            return len == 0;
#endif
        case USPLIT_OP_COMMIT:
            if (len != 2) return false;
            r->crc_lo16 = get16(pl);
            return true;
        case USPLIT_OP_SETTINGS_RESET:
            // D44: an optional mode byte, 0 defaults or 1 blank.
            if ((len != 4 && len != 5) || pl[0] != 'R' || pl[1] != 'S' || pl[2] != 'E' || pl[3] != 'T') return false;
            r->mode = len == 5 ? pl[4] : 0;
            return r->mode <= 1;
        default:
            return false;
    }
}

uint8_t usplit_req_build(uint8_t *buf, uint8_t op, uint8_t seq, const uint8_t *payload, uint8_t payload_len) {
    usplit_req_t r;
    memset(&r, 0, sizeof(r));
    if (USPLIT_REQ_HDR + payload_len + USPLIT_CRC_BYTES > (op == USPLIT_OP_PAGE2 ? USPLIT_MSG2_MAX : USPLIT_MSG_MAX)) return 0;
    if (!payload_ok(op, payload, payload_len, &r)) return 0;
    buf[0] = op;
    buf[1] = seq;
    if (payload_len) memcpy(buf + USPLIT_REQ_HDR, payload, payload_len);
    uint8_t len = USPLIT_REQ_HDR + payload_len;
    put16(buf + len, usplit_crc16(buf, len));
    return len + USPLIT_CRC_BYTES;
}

uint8_t usplit_req_begin(uint8_t *buf, uint8_t seq, uint8_t off, const uint8_t *data, uint8_t n) {
    uint8_t pl[2 + USPLIT_BEGIN_DATA_MAX];
    if (n > USPLIT_BEGIN_DATA_MAX) return 0;
    pl[0] = off;
    pl[1] = n;
    if (n) memcpy(pl + 2, data, n);
    return usplit_req_build(buf, USPLIT_OP_BEGIN, seq, pl, (uint8_t)(2 + n));
}

uint8_t usplit_req_page(uint8_t *buf, uint8_t seq, uint32_t off, const uint8_t *data, uint8_t n) {
    uint8_t pl[4 + USPLIT_PAGE_DATA_MAX];
    if (n > USPLIT_PAGE_DATA_MAX || off > 0xFFFFFF) return 0;
    put24(pl, off);
    pl[3] = n;
    if (n) memcpy(pl + 4, data, n);
    return usplit_req_build(buf, USPLIT_OP_PAGE, seq, pl, (uint8_t)(4 + n));
}

uint8_t usplit_req_page2(uint8_t *buf, uint8_t seq, uint32_t off, const uint8_t *data, uint8_t n) {
    uint8_t pl[4 + USPLIT_PAGE2_DATA_MAX];
    if (n > USPLIT_PAGE2_DATA_MAX || off > 0xFFFFFF) return 0;
    put24(pl, off);
    pl[3] = n;
    if (n) memcpy(pl + 4, data, n);
    return usplit_req_build(buf, USPLIT_OP_PAGE2, seq, pl, (uint8_t)(4 + n));
}

uint8_t usplit_req_commit(uint8_t *buf, uint8_t seq, uint16_t crc_lo16) {
    uint8_t pl[2];
    put16(pl, crc_lo16);
    return usplit_req_build(buf, USPLIT_OP_COMMIT, seq, pl, 2);
}

update_status_t usplit_req_parse(const uint8_t *buf, uint8_t len, usplit_req_t *r) {
    memset(r, 0, sizeof(*r));
    // Only PAGE2 may be longer than USPLIT_MSG_MAX.
    if (len < USPLIT_REQ_HDR + USPLIT_CRC_BYTES || len > USPLIT_MSG2_MAX || (len > USPLIT_MSG_MAX && buf[0] != USPLIT_OP_PAGE2)) {
        if (len >= 1) r->op = buf[0];
        if (len >= 2) r->seq = buf[1];
        return UPDATE_INVALID;
    }
    r->op          = buf[0];
    r->seq         = buf[1];
    r->payload     = buf + USPLIT_REQ_HDR;
    r->payload_len = (uint8_t)(len - USPLIT_REQ_HDR - USPLIT_CRC_BYTES);
    r->crc         = get16(buf + len - USPLIT_CRC_BYTES);
    if (r->crc != usplit_crc16(buf, len - USPLIT_CRC_BYTES)) return UPDATE_INVALID;
    if (!payload_ok(r->op, r->payload, r->payload_len, r)) return UPDATE_INVALID;
    return UPDATE_OK;
}

uint8_t usplit_rsp_build(uint8_t *buf, uint8_t cap, uint8_t status, uint8_t op, uint8_t seq, uint8_t state, const uint8_t *payload) {
    int n = usplit_rsp_payload_len(op);
    if (n < 0) return 0;
    uint8_t len = usplit_rsp_len(op);
    if (cap < len) return 0;
    buf[0] = status;
    buf[1] = op;
    buf[2] = seq;
    buf[3] = state;
    if (payload) {
        memcpy(buf + USPLIT_RSP_HDR, payload, (size_t)n);
    } else {
        memset(buf + USPLIT_RSP_HDR, 0, (size_t)n);
    }
    put16(buf + len - USPLIT_CRC_BYTES, usplit_crc16(buf, len - USPLIT_CRC_BYTES));
    return len;
}

update_status_t usplit_rsp_parse(const uint8_t *buf, uint8_t len, uint8_t op, uint8_t seq, usplit_rsp_t *r) {
    memset(r, 0, sizeof(*r));
    uint8_t want = usplit_rsp_len(op);
    if (!want || len != want) return UPDATE_INVALID;
    if (get16(buf + len - USPLIT_CRC_BYTES) != usplit_crc16(buf, len - USPLIT_CRC_BYTES)) return UPDATE_INVALID;
    r->status      = buf[0];
    r->op          = buf[1];
    r->seq         = buf[2];
    r->state       = buf[3];
    r->payload     = buf + USPLIT_RSP_HDR;
    r->payload_len = (uint8_t)(len - USPLIT_RSP_HDR - USPLIT_CRC_BYTES);
    if (r->op != op || r->seq != seq) return UPDATE_INVALID;
    return UPDATE_OK;
}

// ---- PAGE fragments ------------------------------------------------------------

void usplit_pages_init(usplit_pages_t *a, uint32_t image_len) {
    memset(a, 0, sizeof(*a));
    a->image_len = image_len;
}

update_status_t usplit_pages_accept(usplit_pages_t *a, uint32_t off, const uint8_t *data, uint8_t n, uint32_t *page_done) {
    *page_done = UINT32_MAX;
    if (n == 0 || n > USPLIT_PAGE_DATA_MAX || (off % USPLIT_PAGE_BYTES) + n > USPLIT_PAGE_BYTES) return UPDATE_INVALID;
    if (off + n <= a->next) return UPDATE_OK; // a repeat: acknowledged, not written again
    if (off != a->next) return UPDATE_OUT_OF_ORDER;
    if (n > a->image_len || off > a->image_len - n) return UPDATE_OVERRUN;
    memcpy(a->page + off % USPLIT_PAGE_BYTES, data, n);
    a->next = off + n;
    if (a->next % USPLIT_PAGE_BYTES == 0) *page_done = a->next - USPLIT_PAGE_BYTES;
    return UPDATE_OK;
}

update_status_t usplit_pages_accept2(usplit_pages_t *a, uint32_t off, const uint8_t *data, uint8_t n, usplit_program_t program) {
    if (n == 0 || n > USPLIT_PAGE2_DATA_MAX) return UPDATE_INVALID;
    if (off + n <= a->next) return UPDATE_OK; // a repeat: acknowledged, not written again
    if (off != a->next) return UPDATE_OUT_OF_ORDER;
    if (n > a->image_len || off > a->image_len - n) return UPDATE_OVERRUN;
    uint32_t at    = off % USPLIT_PAGE_BYTES;
    uint32_t first = USPLIT_PAGE_BYTES - at < n ? USPLIT_PAGE_BYTES - at : n;
    memcpy(a->page + at, data, first);
    if ((at + first) % USPLIT_PAGE_BYTES == 0) {
        // The page is complete (n is under a page: at most one per fragment).
        update_status_t st = program(off - at, a->page);
        if (st != UPDATE_OK) return st; // nothing taken; the bytes before off are as they were
        memcpy(a->page, data + first, n - first);
    }
    a->next = off + n;
    return UPDATE_OK;
}
