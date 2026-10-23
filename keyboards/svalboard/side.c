// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Which half this is, in the one-image ("either hand") builds (see side.h).

#include <string.h>
#include "side.h"

// The strap classes this file reads (enum sval_thumb_hand, svalboard.h).
#define STRAP_LEFT 1
#define STRAP_RIGHT 4

// ---- the record and the boot-time choice (host-tested: tests/sval_updater/test_side.c) ----

uint32_t sval_side_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static bool side_value_ok(uint8_t side) {
    return side == SVAL_SIDE_LEFT || side == SVAL_SIDE_RIGHT || side == SVAL_SIDE_UNKNOWN;
}

bool sval_side_record_valid(const sval_side_record_t *r) {
    return r && r->magic == SVAL_SIDE_MAGIC && r->version == SVAL_SIDE_VERSION && r->length == sizeof(*r) && side_value_ok(r->side) && r->source <= SVAL_SIDE_SRC_STRAP && r->crc == sval_side_crc32((const uint8_t *)r, offsetof(sval_side_record_t, crc));
}

void sval_side_record_make(sval_side_record_t *r, uint8_t side, uint8_t source, uint32_t seq) {
    memset(r, 0, sizeof(*r));
    r->magic   = SVAL_SIDE_MAGIC;
    r->version = SVAL_SIDE_VERSION;
    r->length  = sizeof(*r);
    r->side    = side;
    r->source  = source;
    r->seq     = seq;
    r->crc     = sval_side_crc32((const uint8_t *)r, offsetof(sval_side_record_t, crc));
}

const sval_side_record_t *sval_side_newest(const sval_side_record_t *a, const sval_side_record_t *b, bool *write_b) {
    bool a_ok = sval_side_record_valid(a), b_ok = sval_side_record_valid(b);
    if (a_ok && b_ok) {
        bool a_newer = a->seq >= b->seq;
        *write_b     = a_newer;
        return a_newer ? a : b;
    }
    *write_b = a_ok; // replace the copy that is not valid
    return a_ok ? a : b_ok ? b : NULL;
}

uint8_t sval_side_from_eeconfig(uint16_t magic, uint8_t handedness) {
    if (magic != 0xFEE2u && magic != 0xB1A0u && magic != 0xFEE3u) return SVAL_SIDE_UNKNOWN;
    // QMK writes !!is_left, so a per-hand build leaves exactly 0 or 1.
    if (handedness == 1) return SVAL_SIDE_LEFT;
    if (handedness == 0) return SVAL_SIDE_RIGHT;
    return SVAL_SIDE_UNKNOWN;
}

uint8_t sval_side_from_vial_store(sval_side_word_fn word, void *ctx) {
    const uint32_t logical = SVAL_SIDE_VIAL_STORE_LOGICAL, backing = SVAL_SIDE_VIAL_STORE_BACKING;
    uint8_t        head[16];
    uint64_t       hash = 0xcbf29ce484222325ull; // FNV-1a 64 over the consolidated image
    for (uint32_t a = 0; a < logical; a += 2) {
        const uint16_t w  = word(a, ctx);
        const uint8_t  lo = w & 0xFF, hi = w >> 8;
        if (a < sizeof(head)) head[a] = lo, head[a + 1] = hi;
        hash = (hash ^ lo) * 0x100000001b3ull;
        hash = (hash ^ hi) * 0x100000001b3ull;
    }
    uint64_t sum = 0;
    for (uint32_t k = 0; k < 4; k++) sum |= (uint64_t)word(logical + 2 * k, ctx) << (16 * k);
    if (sum != hash) memset(head, 0, sizeof(head)); // QMK starts from zeros, then replays the log
    for (uint32_t a = logical + 8; a + 2 <= backing;) {
        uint8_t        e[8] = {0};
        const uint16_t w0   = word(a, ctx);
        if (w0 == 0) break; // the first empty slot ends the log
        a += 2;
        e[0] = w0 & 0xFF, e[1] = w0 >> 8;
        const uint8_t type = e[0] >> 6;
        if (type == 0) { // multi-byte: a 19-bit address, 1-5 bytes
            const uint16_t w1 = word(a, ctx);
            a += 2;
            e[2] = w1 & 0xFF, e[3] = w1 >> 8;
            const uint32_t addr = ((uint32_t)(e[0] & 7) << 16) | ((uint32_t)e[1] << 8) | e[2];
            const uint8_t  len  = (e[0] >> 3) & 7;
            if (len > 1) {
                const uint16_t w2 = word(a, ctx);
                a += 2;
                e[4] = w2 & 0xFF, e[5] = w2 >> 8;
            }
            if (len > 3) {
                const uint16_t w3 = word(a, ctx);
                a += 2;
                e[6] = w3 & 0xFF, e[7] = w3 >> 8;
            }
            if (len == 0 || len > 5 || addr + len > logical) break; // corrupt: stop where QMK would
            for (uint8_t i = 0; i < len; i++)
                if (addr + i < sizeof(head)) head[addr + i] = e[3 + i];
        } else if (type == 1) { // one byte below address 64
            const uint32_t addr = e[0] & 0x3F;
            if (addr < sizeof(head)) head[addr] = e[1];
        } else if (type == 2) { // a 0 or 1 written as a word
            const uint32_t addr = ((uint32_t)(e[0] & 0x1F) << 9) | ((uint32_t)e[1] << 1);
            if (addr + 1 >= logical) break;
            if (addr < sizeof(head)) head[addr] = (e[0] >> 5) & 1;
            if (addr + 1 < sizeof(head)) head[addr + 1] = 0;
        } else {
            break;
        }
    }
    return sval_side_from_eeconfig((uint16_t)(head[0] | (head[1] << 8)), head[SVAL_SIDE_EECONFIG_HANDEDNESS]);
}

uint8_t sval_side_from_strap(uint8_t strap_class) {
    return strap_class == STRAP_LEFT ? SVAL_SIDE_LEFT : strap_class == STRAP_RIGHT ? SVAL_SIDE_RIGHT : SVAL_SIDE_UNKNOWN;
}

void sval_side_decide(uint8_t strap_side, const sval_side_record_t *rec, uint8_t migrated, sval_side_decision_t *d) {
    memset(d, 0, sizeof(*d));
    if (strap_side != SVAL_SIDE_UNKNOWN) {
        d->side   = strap_side;
        d->source = SVAL_SIDE_SRC_STRAP;
        if (!rec || rec->side != strap_side) {
            d->write        = true;
            d->write_side   = strap_side;
            d->write_source = SVAL_SIDE_SRC_STRAP;
        }
        return;
    }
    if (rec) {
        d->side   = rec->side;
        d->source = rec->side == SVAL_SIDE_UNKNOWN ? SVAL_SIDE_SRC_NONE : rec->source;
        return;
    }
    // No record at all: the first boot of an either build on this board. Adopt
    // what a per-hand build left, or record that there was nothing, so a later
    // boot never reads the formatted settings as a side.
    d->side         = migrated;
    d->source       = migrated == SVAL_SIDE_UNKNOWN ? SVAL_SIDE_SRC_NONE : SVAL_SIDE_SRC_MIGRATED;
    d->write        = true;
    d->write_side   = migrated;
    d->write_source = d->source;
}

uint8_t sval_side_host_check(uint8_t strap_side, uint8_t side, uint8_t source) {
    if (!side_value_ok(side)) return SVAL_SIDE_INVALID;
    if (side == SVAL_SIDE_UNKNOWN ? source != SVAL_SIDE_SRC_NONE : (source != SVAL_SIDE_SRC_FACTORY && source != SVAL_SIDE_SRC_HOST)) return SVAL_SIDE_INVALID;
    if (strap_side != SVAL_SIDE_UNKNOWN && side != strap_side) return SVAL_SIDE_STRAP;
    return SVAL_SIDE_OK;
}

uint8_t sval_side_pair(uint8_t mine, bool other_connected, bool other_reported, uint8_t other) {
    if (!other_connected) return SVAL_SIDE_PAIR_OK;
    if (!other_reported) return SVAL_SIDE_PAIR_PENDING;
    if (mine == SVAL_SIDE_UNKNOWN || other == SVAL_SIDE_UNKNOWN) return SVAL_SIDE_PAIR_UNKNOWN;
    return mine == other ? SVAL_SIDE_PAIR_SAME : SVAL_SIDE_PAIR_OK;
}

#ifndef SVAL_SIDE_HOST_TEST
// ---- firmware ------------------------------------------------------------------

#    include "quantum.h"
#    include "eeprom.h"
#    include "svalboard.h"
#    include "identity.h"
#    include "build_reset.h"
#    include "split_common/transactions.h"
#    include "split_common/split_util.h"
#    include "hardware/regs/addressmap.h"
#    include "hardware/structs/sio.h"

#    ifndef SVAL_EITHER_HAND
#        error "side.c is for the either-hand builds (SVAL_EITHER_HAND)"
#    endif
#    if !defined(SERIAL_USART_PINS_RUNTIME) || !defined(SERIAL_USART_FULL_DUPLEX)
#        error "the either-hand builds choose the split link pins at run time (SERIAL_USART_PINS_RUNTIME, full duplex)"
#    endif
_Static_assert(SVAL_SIDE_SECTOR_B + 4096u == BUILD_RESET_SECTOR, "the side sectors are just below the build reset sector");
#    ifdef SVAL_UPDATE_BASE
_Static_assert(SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE <= SVAL_SIDE_SECTOR_A, "the staging slot ends below the side sectors");
#    endif
_Static_assert(sizeof(sval_side_record_t) == 16, "the side record is 16 bytes");

#    define ROWS_PER_HAND (MATRIX_ROWS / 2)

static void link_search_task(void);

static uint8_t side = SVAL_SIDE_UNKNOWN, source = SVAL_SIDE_SRC_NONE;
static bool    other_connected, other_reported;
static uint8_t other_side = SVAL_SIDE_UNKNOWN;
static uint32_t restart_at;
// A write the other half asked for over the link (done from the main loop: the
// flash write keeps interrupts off for about 50 ms).
static bool    rpc_pending;
static uint8_t rpc_side, rpc_source, rpc_flags;

static const sval_side_record_t *record_at(uint32_t offset) {
    return (const sval_side_record_t *)(XIP_NOCACHE_NOALLOC_BASE + offset);
}

static const sval_side_record_t *stored(bool *write_b) {
    bool dummy;
    if (!identity_available()) return NULL;
    return sval_side_newest(record_at(SVAL_SIDE_SECTOR_A), record_at(SVAL_SIDE_SECTOR_B), write_b ? write_b : &dummy);
}

static uint8_t save(uint8_t s, uint8_t src) {
    if (!identity_available()) return SVAL_SIDE_UNAVAILABLE;
    bool                      write_b;
    const sval_side_record_t *n = stored(&write_b);
    sval_side_record_t        r;
    sval_side_record_make(&r, s, src, n ? n->seq + 1 : 1);
    return identity_side_record_write(write_b ? SVAL_SIDE_SECTOR_B : SVAL_SIDE_SECTOR_A, &r, sizeof(r)) ? SVAL_SIDE_OK : SVAL_SIDE_WRITE_FAILED;
}

static uint16_t vial_store_word(uint32_t off, void *ctx) {
    (void)ctx;
    return (uint16_t)~*(const volatile uint16_t *)(XIP_NOCACHE_NOALLOC_BASE + SVAL_SIDE_VIAL_STORE_BASE + off);
}

// The side a Vial board's old store recorded, read before anything can write
// flash: this firmware's settings store has its second copy at 0x1E0000, so on
// a Vial board eeprom_driver_init() rewrites the old store before
// keyboard_pre_init_kb() runs. board_init() runs from halInit(), before that.
static uint8_t vial_side = SVAL_SIDE_UNKNOWN;

// Plain XIP reads only (about 64 KB): no flash commands this early.
void board_init(void) {
    vial_side = sval_side_from_vial_store(vial_store_word, NULL);
}

void sval_side_init(void) {
    const uint8_t             strap    = sval_side_from_strap(sval_thumb_hand());
    const sval_side_record_t *rec      = stored(NULL);
    uint8_t                   migrated = SVAL_SIDE_UNKNOWN;
    // The handedness byte is read before build_reset_pre_init() can format the
    // settings, and only when there is a record area to keep the result in.
    // svalboard/qmk boards (x3 and older per-hand builds) left it in this
    // firmware's store; a Vial board left no valid store here, so its side comes
    // from the old Vial store, read at board_init(). A valid store here never
    // falls back to the Vial area, which now holds this firmware's second copy.
    if (!rec && identity_available()) {
        const uint16_t magic = eeprom_read_word((const uint16_t *)0);
        migrated             = sval_side_from_eeconfig(magic, eeprom_read_byte((const uint8_t *)SVAL_SIDE_EECONFIG_HANDEDNESS));
        if (migrated == SVAL_SIDE_UNKNOWN && sval_side_from_eeconfig(magic, 0) == SVAL_SIDE_UNKNOWN) migrated = vial_side;
    }
    sval_side_decision_t d;
    sval_side_decide(strap, rec, migrated, &d);
    side   = d.side;
    source = d.source;
    if (d.write && identity_available()) save(d.write_side, d.write_source);
}

uint8_t sval_side(void) {
    return side;
}
uint8_t sval_side_source(void) {
    return source;
}
bool sval_side_known(void) {
    return side != SVAL_SIDE_UNKNOWN;
}

// split_pre_init(): the side for the matrix offsets, the LED range and the link.
// An unknown half runs as the right half's placeholder; its keys are dropped.
bool is_keyboard_left_impl(void) {
    return side == SVAL_SIDE_LEFT;
}

// A presence ping that fails now and then (the other half busy, a noisy cable)
// is not a disconnect: only SIDE_MISSES in a row (1.5 s) drop the other half,
// so its keys are not blocked and released on a single miss.
#    define SIDE_MISSES 3
void sval_side_other_set(bool connected, bool reported, uint8_t s) {
    static uint8_t misses = 0;
    if (!connected && other_connected && ++misses < SIDE_MISSES) return;
    misses          = 0;
    other_connected = connected;
    other_reported  = connected && reported;
    other_side      = other_reported ? s : SVAL_SIDE_UNKNOWN;
}

static uint8_t pair_now(void) {
    return sval_side_pair(side, other_connected, other_reported, other_side);
}

bool sval_side_row_allowed(uint8_t row) {
    if (side == SVAL_SIDE_UNKNOWN) return false;
    uint8_t p = pair_now();
    if (p == SVAL_SIDE_PAIR_SAME) return false; // which one is wrong is not known: neither types
    if ((row < ROWS_PER_HAND) == (side == SVAL_SIDE_LEFT)) return true;
    // The other half's keys only once it has said it is the other side.
    return p == SVAL_SIDE_PAIR_OK && other_connected;
}

bool sval_side_input_blocked(void) {
    return !sval_side_row_allowed(0) || !sval_side_row_allowed(ROWS_PER_HAND);
}

bool pre_process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (IS_KEYEVENT(record->event) && !sval_side_row_allowed(record->event.key.row)) return false;
    return pre_process_record_user(keycode, record);
}

static uint8_t host_write(uint8_t s, uint8_t src, uint8_t flags) {
    uint8_t st = sval_side_host_check(sval_side_from_strap(sval_thumb_hand()), s, src);
    if (st == SVAL_SIDE_OK) st = save(s, src);
    if (st == SVAL_SIDE_OK && (flags & 1)) {
        restart_at = timer_read32() + 100;
        if (!restart_at) restart_at = 1;
    }
    return st;
}

void sval_side_task(void) {
    // Keys held when input becomes blocked would never see their release.
    static uint8_t last_mask = 0xFF;
    uint8_t        mask      = (sval_side_row_allowed(0) ? 1 : 0) | (sval_side_row_allowed(ROWS_PER_HAND) ? 2 : 0);
    if (mask != last_mask) {
        if (last_mask != 0xFF && (last_mask & ~mask)) clear_keyboard();
        last_mask = mask;
    }
    if (rpc_pending) {
        rpc_pending = false;
        host_write(rpc_side, rpc_source, rpc_flags);
    }
    link_search_task();
    if (restart_at && timer_expired32(timer_read32(), restart_at)) {
        restart_at = 0;
        soft_reset_keyboard();
    }
}

// ---- the split link pins ----------------------------------------------------------
//
// Both halves wire GPIO0 and GPIO1 the same way and the cable is straight, so
// one half must transmit on GPIO0 and the other on GPIO1. The per-hand builds
// fix it: left TX GPIO0, right TX GPIO1. Here the half with USB uses its side's
// pins (an unknown one, the left's); the other half listens first and takes
// the line the master transmits on as its RX. So the link comes up whatever
// the other half's side, which lets the master learn that side (and report two
// halves that claim the same one), and still pairs with a per-hand half.

static uint8_t link_tx = 0xFF, link_how = 0;

uint8_t sval_side_link_tx(uint8_t *how) {
    if (how) *how = link_how;
    return link_tx;
}

#    ifndef SVAL_LINK_LISTEN_MS
#        define SVAL_LINK_LISTEN_MS 1500 // the master retries a link that is down every 500 ms
#    endif
#    define LINK_MIN_FALLS 2 // the master sends one byte per try while the link is down (one every 500 ms)

// Which of GPIO0 (0) or GPIO1 (1) carries the master's transmissions, or -1.
// A UART line idles high; the master's RX idles high under our pull-up, and
// its TX shows start bits: LINK_MIN_FALLS falling edges on one line while the
// other never goes low. A master still in reset pulls both lines down (RP2040
// pad default): lows on both lines start the count again.
static int8_t link_listen(uint32_t ms) {
    gpio_set_pin_input_high(GP0);
    gpio_set_pin_input_high(GP1);
    wait_us(100);
    uint32_t start = timer_read32();
    uint16_t falls[2] = {0, 0};
    bool     low[2]   = {false, false};
    uint32_t prev     = 3;
    while (timer_elapsed32(start) < ms) {
        uint32_t now = sio_hw->gpio_in & 3u;
        for (uint8_t p = 0; p < 2; p++) {
            if (!(now & (1u << p))) {
                low[p] = true;
                if ((prev & (1u << p)) && falls[p] < 0xFFFF) falls[p]++;
            }
        }
        prev = now;
        if (low[0] && low[1]) {
            falls[0] = falls[1] = 0;
            low[0] = low[1] = false;
            continue;
        }
        for (uint8_t p = 0; p < 2; p++) {
            if (falls[p] >= LINK_MIN_FALLS && !low[p ^ 1]) return (int8_t)p;
        }
    }
    return -1;
}

// platforms/chibios/drivers/vendor/RP/RP2040/serial_vendor.c
void serial_usart_pins_kb(bool master, pin_t *tx, pin_t *rx) {
    bool tx_on_gp0;
    if (master) {
        tx_on_gp0 = side != SVAL_SIDE_RIGHT;
        link_how  = side == SVAL_SIDE_UNKNOWN ? 2 : 0;
    } else {
        int8_t master_tx = link_listen(SVAL_LINK_LISTEN_MS);
        if (master_tx >= 0) {
            tx_on_gp0 = master_tx == 1;
            link_how  = 1;
        } else {
            // Nothing heard: the side's pins, as a per-hand build (an unknown one, the right's).
            tx_on_gp0 = side == SVAL_SIDE_LEFT;
            link_how  = side == SVAL_SIDE_UNKNOWN ? 2 : 0;
        }
    }
    *tx     = tx_on_gp0 ? GP0 : GP1;
    *rx     = tx_on_gp0 ? GP1 : GP0;
    link_tx = tx_on_gp0 ? 0 : 1;
}

// The half with USB, while the link is down: every LINK_SEARCH_MS it moves to
// the other pin pair and back. The other half may transmit on our TX line: a
// half that claims our side and could not hear us (older either builds), or a
// per-hand build of our side. On the swapped pins the link comes up, and its
// side report shows the pair for what it is (side.h, pair SAME).
#    ifndef LINK_SEARCH_MS
#        define LINK_SEARCH_MS 2000
#    endif
void serial_transport_repin(pin_t tx_pin, pin_t rx_pin);
static void link_search_task(void) {
    static uint32_t down_since = 0;
    static bool     was_up     = false;
    if (!is_keyboard_master() || link_tx == 0xFF) return;
    if (is_transport_connected()) {
        was_up     = true;
        down_since = 0;
        return;
    }
    if (!down_since || was_up) {
        down_since = timer_read32();
        was_up     = false;
        return;
    }
    if (timer_elapsed32(down_since) < LINK_SEARCH_MS) return;
    down_since = timer_read32();
    bool tx_on_gp0 = link_tx != 0;
    serial_transport_repin(tx_on_gp0 ? GP0 : GP1, tx_on_gp0 ? GP1 : GP0);
    link_tx  = tx_on_gp0 ? 0 : 1;
    link_how = 3;
}

// ---- host protocol: Sval 0x33 (docs/protocol.md, "Side") ---------------------------
//
// data[0] op, data[1] status; the rest per op. Ops: 0 GET, 1 SET, 2 CLEAR.

enum { SIDE_OP_GET = 0, SIDE_OP_SET = 1, SIDE_OP_CLEAR = 2 };
#    define SIDE_FORMAT 1
#    define SIDE_ACCEPTED 7 // the other half took the write; it saves it from its main loop

static uint8_t relay(uint8_t op, uint8_t s, uint8_t src, uint8_t flags) {
    if (!is_keyboard_master() || !sval_other_half_connected()) return SVAL_SIDE_NOT_CONNECTED;
    uint8_t req[5] = {SVAL_SIDE_RPC_OP, op, s, src, flags};
    uint8_t rsp[2] = {0, 0};
    if (!transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(rsp), rsp) || rsp[0] != SVAL_SIDE_RPC_OP) return SVAL_SIDE_NO_ANSWER;
    return rsp[1];
}

bool sval_side_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 5 || in[0] != SVAL_SIDE_RPC_OP || out_len < 2) return false;
    uint8_t st = sval_side_host_check(sval_side_from_strap(sval_thumb_hand()), in[2], in[3]);
    if (st == SVAL_SIDE_OK && !identity_available()) st = SVAL_SIDE_UNAVAILABLE;
    if (st == SVAL_SIDE_OK) {
        rpc_side    = in[2];
        rpc_source  = in[3];
        rpc_flags   = in[4];
        rpc_pending = true;
        st          = SIDE_ACCEPTED;
    }
    out[0] = SVAL_SIDE_RPC_OP;
    out[1] = st;
    return true;
}

void sval_side_command(uint8_t *d, uint8_t length) {
    if (length < 16) return;
    const uint8_t op = d[0];
    if (op == SIDE_OP_GET) {
        memset(&d[1], 0, length - 1);
        const sval_side_record_t *rec = stored(NULL);
        uint8_t                   how;
        d[1]  = SVAL_SIDE_OK;
        d[2]  = SIDE_FORMAT;
        d[3]  = side;
        d[4]  = source;
        d[5]  = sval_thumb_hand();
        d[6]  = sval_thumb_hand_raw();
        d[7]  = rec ? rec->side : 0xFE; // 0xFE: no record (or no record area)
        d[8]  = rec ? rec->source : 0;
        d[9]  = (other_connected ? 1 : 0) | (other_reported ? 2 : 0);
        d[10] = other_side;
        d[11] = pair_now();
        d[12] = sval_side_link_tx(&how);
        d[13] = how;
        d[14] = (sval_side_input_blocked() ? 1 : 0) | (rec && rec->side != side ? 2 : 0) | (identity_available() ? 4 : 0) | (is_keyboard_master() ? 8 : 0);
        return;
    }
    if (op != SIDE_OP_SET && op != SIDE_OP_CLEAR) {
        d[1] = SVAL_SIDE_INVALID;
        return;
    }
    const uint8_t target = d[1];
    const uint8_t s      = op == SIDE_OP_SET ? d[2] : SVAL_SIDE_UNKNOWN;
    const uint8_t src    = op == SIDE_OP_SET ? d[3] : SVAL_SIDE_SRC_NONE;
    const uint8_t flags  = op == SIDE_OP_SET ? d[4] : d[2];
    uint8_t       st;
    if (op == SIDE_OP_SET && s == SVAL_SIDE_UNKNOWN) {
        st = SVAL_SIDE_INVALID; // clearing is its own op
    } else if (target == 0) {
        st = host_write(s, src, flags);
    } else if (target == 1) {
        st = relay(op, s, src, flags);
    } else {
        st = SVAL_SIDE_INVALID;
    }
    memset(&d[1], 0, length - 1);
    d[1] = st;
}
#endif
