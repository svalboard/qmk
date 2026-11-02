// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad (SVAL_NATIVE_PTP).
//
// The USB half merges both IQS5xx pads into one surface and reports it on its
// own HID interface (Digitizers 0x0D, Touch Pad 0x05). Windows or Linux then
// own gesture recognition. This file owns only what is fed in: geometry,
// contact IDs, arming, the 5-contact limit, lift, palm, faults and pacing.
//
// Threads: the feature-report setters run in the USB ISR and only store
// values (f_*). Everything else runs in the main loop.
#ifdef SVAL_PTP_HOST_TEST
#    include "ptp_test_platform.h"
#else
#    include <ch.h>
#    include <hal.h>
#    include <string.h>
#    include "usb_main.h"
#    include "usb_driver.h"
#    include "usb_endpoints.h"
#endif
#include "ptp.h"
#include "ptp_descriptor.h"

// The interface's report descriptor, for tmk_core (usb_ptp.h).
#ifndef SVAL_PTP_HOST_TEST
#    include "usb_ptp.h"
const uint8_t SvalPtpReport[SVAL_PTP_REPORT_DESCRIPTOR_SIZE] = {SVAL_PTP_REPORT_DESCRIPTOR};
_Static_assert(sizeof((const uint8_t[]){SVAL_PTP_REPORT_DESCRIPTOR}) == SVAL_PTP_REPORT_DESCRIPTOR_SIZE, "usb_ptp.h: descriptor size");
#endif

#ifndef SVAL_PTP_HOST_TEST
#    if CH_CFG_ST_FREQUENCY != 1000000
#        error "ptp.c counts scan time on a 1 MHz system timer"
#    endif
extern usb_endpoint_in_t usb_endpoints_in[USB_ENDPOINT_IN_COUNT];

static uint32_t ptp_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

// Post one whole report or nothing; never wait. QMK's usb_endpoint_in_send
// resets and retries the queue even with TIME_IMMEDIATE.
static bool ptp_try_send(const uint8_t *report) {
    usb_endpoint_in_t      *endpoint = &usb_endpoints_in[USB_ENDPOINT_IN_PTP];
    output_buffers_queue_t *q        = &endpoint->obqueue;
    bool                    sent     = false;
    osalSysLock();
    if (usbGetDriverStateI(endpoint->config.usbp) == USB_ACTIVE && !q->suspended && q->ptr == NULL && !obqIsFullI(q) && obqGetEmptyBufferTimeoutS(q, TIME_IMMEDIATE) == MSG_OK) {
        memcpy(q->ptr, report, SVAL_PTP_INPUT_SIZE);
        obqPostFullBufferS(q, SVAL_PTP_INPUT_SIZE);
        sent = true;
    }
    osalSysUnlock();
    return sent;
}
#    define PTP_LOCK() osalSysLock()
#    define PTP_UNLOCK() osalSysUnlock()
#endif

// Microsoft's published default certification blob for Precision Touchpad
// development (Windows Precision Touchpad sample descriptor, feature report
// 0xC5). Not a Svalboard certificate; it only satisfies the inbox driver.
static const uint8_t cert[SVAL_PTP_CERT_SIZE] = {
    SVAL_PTP_REPORT_ID_CERT,
    0xFC,
    0x28,
    0xFE,
    0x84,
    0x40,
    0xCB,
    0x9A,
    0x87,
    0x0D,
    0xBE,
    0x57,
    0x3C,
    0xB6,
    0x70,
    0x09,
    0x88,
    0x07,
    0x97,
    0x2D,
    0x2B,
    0xE3,
    0x38,
    0x34,
    0xB6,
    0x6C,
    0xED,
    0xB0,
    0xF7,
    0xE5,
    0x9C,
    0xF6,
    0xC2,
    0x2E,
    0x84,
    0x1B,
    0xE8,
    0xB4,
    0x51,
    0x78,
    0x43,
    0x1F,
    0x28,
    0x4B,
    0x7C,
    0x2D,
    0x53,
    0xAF,
    0xFC,
    0x47,
    0x70,
    0x1B,
    0x59,
    0x6F,
    0x74,
    0x43,
    0xC4,
    0xF3,
    0x47,
    0x18,
    0x53,
    0x1A,
    0xA2,
    0xA1,
    0x71,
    0xC7,
    0x95,
    0x0E,
    0x31,
    0x55,
    0x21,
    0xD3,
    0xB5,
    0x1E,
    0xE9,
    0x0C,
    0xBA,
    0xEC,
    0xB8,
    0x89,
    0x19,
    0x3E,
    0xB3,
    0xAF,
    0x75,
    0x81,
    0x9D,
    0x53,
    0xB9,
    0x41,
    0x57,
    0xF4,
    0x6D,
    0x39,
    0x25,
    0x29,
    0x7C,
    0x87,
    0xD9,
    0xB4,
    0x98,
    0x45,
    0x7D,
    0xA7,
    0x26,
    0x9C,
    0x65,
    0x3B,
    0x85,
    0x68,
    0x89,
    0xD7,
    0x3B,
    0xBD,
    0xFF,
    0x14,
    0x67,
    0xF2,
    0x2B,
    0xF0,
    0x2A,
    0x41,
    0x54,
    0xF0,
    0xFD,
    0x2C,
    0x66,
    0x7C,
    0xF8,
    0xC0,
    0x8F,
    0x33,
    0x13,
    0x03,
    0xF1,
    0xD3,
    0xC1,
    0x0B,
    0x89,
    0xD9,
    0x1B,
    0x62,
    0xCD,
    0x51,
    0xB7,
    0x80,
    0xB8,
    0xAF,
    0x3A,
    0x10,
    0xC1,
    0x8A,
    0x5B,
    0xE8,
    0x8A,
    0x56,
    0xF0,
    0x8C,
    0xAA,
    0xFA,
    0x35,
    0xE9,
    0x42,
    0xC4,
    0xD8,
    0x55,
    0xC3,
    0x38,
    0xCC,
    0x2B,
    0x53,
    0x5C,
    0x69,
    0x52,
    0xD5,
    0xC8,
    0x73,
    0x02,
    0x38,
    0x7C,
    0x73,
    0xB6,
    0x41,
    0xE7,
    0xFF,
    0x05,
    0xD8,
    0x2B,
    0x79,
    0x9A,
    0xE2,
    0x34,
    0x60,
    0x8F,
    0xA3,
    0x32,
    0x1F,
    0x09,
    0x78,
    0x62,
    0xBC,
    0x80,
    0xE3,
    0x0F,
    0xBD,
    0x65,
    0x20,
    0x08,
    0x13,
    0xC1,
    0xE2,
    0xEE,
    0x53,
    0x2D,
    0x86,
    0x7E,
    0xA7,
    0x5A,
    0xC5,
    0xD3,
    0x7D,
    0x98,
    0xBE,
    0x31,
    0x48,
    0x1F,
    0xFB,
    0xDA,
    0xAF,
    0xA2,
    0xA8,
    0x6A,
    0x89,
    0xD6,
    0xBF,
    0xF2,
    0xD3,
    0x32,
    0x2A,
    0x9A,
    0xE4,
    0xCF,
    0x17,
    0xB7,
    0xB8,
    0xF4,
    0xE1,
    0x33,
    0x08,
    0x24,
    0x8B,
    0xC4,
    0x43,
    0xA5,
    0xE5,
    0x24,
    0xC2,
};

// ---------------------------------------------------------------- state

// Written by the USB ISR (feature reports, reset); read in the main loop.
static volatile uint8_t  f_mode, f_latency, f_switches = 3;
static volatile bool     f_reset;
static volatile uint32_t f_rejected; // SET_REPORT values ignored after the data stage
static bool              host_mode3(void);    // mode 3, unless the touchpad mode keeps the built-in gestures

enum { C_FREE, C_CANDIDATE, C_ACTIVE, C_IGNORED };
typedef struct {
    uint8_t  state, pad, slot, life, cid;
    bool     conf;      // Confidence; once cleared it stays cleared
    bool     sent_down; // reported with Tip = 1 at least once
    bool     lift;      // gone: one Tip = 0 frame (after one Tip = 1 if never sent), then free
    bool     palm;      // chip palm flag: one Confidence = 0 frame, then lift
    uint16_t x, y;
    uint16_t sx, sy; // the position last sent with Tip = 1: a release goes there
    uint32_t order;
} contact_t;
// Worst case: 5 reported contacts lifting while the endpoint is busy, 5 new
// candidates waiting for their slots, and every other chip slot of both pads.
#define POOL 20
static contact_t pool[POOL];
static uint32_t  arrival;
// Contacts in use per pad, so the idle main loop never walks the pool.
static uint8_t pad_used[2];

typedef struct {
    bool     present; // this hand has a pad (sval_ptp_set_pads)
    bool     known, up, armed, has_epoch;
    bool     fresh;     // a sample changed this pad since the last frame
    bool     has_valid; // valid_us holds a time
    bool     resume;    // lifted because its samples stopped: arm on the next valid sample, finger down or not
    uint16_t stale_ms;  // lift after this long without a valid sample (0: SVAL_PTP_STALE_MS)
    uint32_t epoch;
    uint32_t sample_us; // when the sample behind `fresh` was read
    uint32_t valid_us;  // the last valid sample (any answer from the chip)
} pad_state_t;
static pad_state_t pads[2];

static bool     native, paused, mode3, output, hold, release_pending, dirty, ever_sent, scan_started, busy_streak;
static uint32_t release_at, last_sent, scan_clock, scan_rem, frames_sent, send_busy, busy_since;
static uint16_t cid_busy, xid_busy, scan, host_silent, merge_waits, pool_full;
static uint8_t  cid_next, key_buttons, sent_buttons, seen_switches = 3, pad_mask;
#if SVAL_PTP_KEY_BUTTONS
static uint8_t key_taken; // buttons whose press went to the touchpad
#endif
static uint16_t motion[2];
static uint8_t  lost_down[2]; // sval_ptp_lost_down

// Velocity Scroll's grab (sval_ptp_set_grab): one finger per pad, followed by
// slot and lifetime ID. A finger already down when the grab starts is taken
// at once; once it lifts, only a finger that lands later is taken, so one
// resting on the pad never takes over.
typedef struct {
    bool     down;
    uint8_t  slot, life;
    uint8_t  skip[5]; // lifetime ID + 1 per slot of fingers not to take (0: none)
    uint16_t touch;   // serial of the finger followed
    uint16_t x, y;    // its position on this pad, in the touchpad's orientation
} grab_t;
static bool     grabbed;
static grab_t   grab[2];
static uint16_t grab_serial;

static void map_xy(uint8_t pad, const uint8_t *slot, uint16_t *x, uint16_t *y);

static void grab_reset(uint8_t pad) {
    memset(&grab[pad], 0, sizeof(grab[pad]));
}

static void grab_feed(uint8_t pad, uint8_t mask, const uint8_t *f) {
    grab_t *g = &grab[pad];
    if (g->down) {
        const uint8_t *s = f + 16 + 8 * g->slot;
        if (!(mask & (1u << g->slot)) || s[0] != g->life) {
            g->down = false;
            // Every finger still down stays unused until it lifts.
            for (uint8_t slot = 0; slot < 5; slot++)
                g->skip[slot] = (mask & (1u << slot)) ? (uint8_t)(f[16 + 8 * slot] + 1) : 0;
        }
    }
    for (uint8_t slot = 0; slot < 5; slot++)
        if (!(mask & (1u << slot)) || g->skip[slot] != (uint8_t)(f[16 + 8 * slot] + 1)) g->skip[slot] = 0;
    for (uint8_t slot = 0; !g->down && slot < 5; slot++) {
        if (!(mask & (1u << slot)) || g->skip[slot]) continue;
        g->down  = true;
        g->slot  = slot;
        g->life  = f[16 + 8 * slot];
        g->touch = ++grab_serial ? grab_serial : ++grab_serial;
    }
    if (g->down) {
        uint16_t x, y;
        map_xy(pad, f + 16 + 8 * g->slot, &x, &y);
        g->x = x - pad * SVAL_PTP_PAD_STRIDE;
        g->y = y;
    }
}
static uint8_t  last_report[SVAL_PTP_INPUT_SIZE] = {SVAL_PTP_REPORT_ID_INPUT};

static uint16_t ptp_get16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t ptp_get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void ptp_put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
static void ptp_put32(uint8_t *p, uint32_t v) {
    ptp_put16(p, v);
    ptp_put16(p + 2, v >> 16);
}
static void bump16(uint16_t *c) {
    if (*c != 0xFFFF) (*c)++;
}

// GET_REPORT(Input) must never hand the host an old contact: after a reset or
// a dropped release, the last report is all up (its scan time is kept).
static void clear_last_report(void) {
    PTP_LOCK();
    memset(last_report, 0, sizeof(last_report));
    last_report[0] = SVAL_PTP_REPORT_ID_INPUT;
    ptp_put16(last_report + 31, scan);
    PTP_UNLOCK();
}

// --------------------------------------------------------------- contacts

static void free_contact(contact_t *c) {
    if (c->state == C_FREE) return;
    if (c->state == C_ACTIVE) cid_busy &= ~(1u << c->cid);
    if (c->state == C_IGNORED) xid_busy &= ~(1u << c->cid);
    pad_used[c->pad]--;
    memset(c, 0, sizeof(*c));
}

static void clear_all(void) {
    for (uint8_t i = 0; (pad_used[0] || pad_used[1]) && i < POOL; i++)
        free_contact(&pool[i]);
    pads[0].armed = pads[1].armed = false;
    pads[0].fresh = pads[1].fresh = false;
    pads[0].resume = pads[1].resume = false;
    dirty                           = false;
}

static contact_t *alloc_contact(uint8_t pad) {
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_FREE) {
            pool[i].pad = pad;
            pad_used[pad]++;
            return &pool[i];
        }
    bump16(&pool_full);
    return NULL;
}

static uint8_t take_cid(void) {
    for (uint8_t k = 0; k < 16; k++) {
        uint8_t id = (cid_next + k) & 15;
        if (!(cid_busy & (1u << id))) {
            cid_busy |= 1u << id;
            cid_next = (id + 1) & 15; // round robin: a freed ID is not reused at once
            return id;
        }
    }
    return 0; // unreachable: admit() gives at most 5 contacts an ID
}

// IDs for contacts the touchpad leaves out (C_IGNORED): their own space, so
// the touchpad's IDs are exactly as without them. At most 10 (the chip slots).
static uint8_t take_xid(void) {
    for (uint8_t id = 0; id < 16; id++) {
        if (!(xid_busy & (1u << id))) {
            xid_busy |= 1u << id;
            return id;
        }
    }
    return 0; // unreachable
}

static void end_contact(contact_t *c) {
    if (c->state == C_ACTIVE || c->state == C_CANDIDATE) {
        c->lift = true;
        dirty   = true;
    } else {
        free_contact(c);
    }
}

static bool pad_has_contacts(uint8_t pad) {
    return pad_used[pad] != 0;
}

// A contact on this pad is down and not lifting.
static bool pad_down(uint8_t pad) {
    if (!pad_used[pad]) return false;
    for (uint8_t i = 0; i < POOL; i++) {
        const contact_t *c = &pool[i];
        if ((c->state == C_ACTIVE || c->state == C_CANDIDATE) && c->pad == pad && !c->lift) return true;
    }
    return false;
}

// Reset, recalibration, a dead pad or a lost half: lift only that pad.
static void lift_pad(uint8_t pad) {
    for (uint8_t i = 0; pad_used[pad] && i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad) continue;
        if (c->state == C_ACTIVE && c->sent_down) {
            c->lift = true;
            c->palm = false;
            dirty   = true;
        } else {
            free_contact(c);
        }
    }
    pads[pad].armed = false;
}

// The chip's palm or too-many flag. Its frame carries no positions, so report
// the reported contacts once with Confidence = 0, then lift them.
static void palm_pad(uint8_t pad) {
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad) continue;
        if (c->state == C_ACTIVE && c->sent_down) {
            c->conf = false;
            if (!c->lift) c->palm = true;
            dirty = true;
        } else {
            free_contact(c);
        }
    }
    pads[pad].armed = false;
}

static void map_xy(uint8_t pad, const uint8_t *slot, uint16_t *x, uint16_t *y) {
    uint16_t rx = ptp_get16(slot + 1), ry = ptp_get16(slot + 3);
    if (rx > SVAL_PTP_PAD_X - 1) rx = SVAL_PTP_PAD_X - 1;
    if (ry > SVAL_PTP_PAD_Y - 1) ry = SVAL_PTP_PAD_Y - 1;
    if (pad ? SVAL_PTP_FLIP_R_X : SVAL_PTP_FLIP_L_X) rx = SVAL_PTP_PAD_X - 1 - rx;
    if (pad ? SVAL_PTP_FLIP_R_Y : SVAL_PTP_FLIP_L_Y) ry = SVAL_PTP_PAD_Y - 1 - ry;
    *x = rx + pad * SVAL_PTP_PAD_STRIDE;
    *y = ry;
}

static uint16_t distance(const contact_t *c, uint16_t x, uint16_t y) {
    uint16_t dx = c->x > x ? c->x - x : x - c->x;
    uint16_t dy = c->y > y ? c->y - y : y - c->y;
    return dx + dy;
}

static contact_t *find_contact(uint8_t pad, uint8_t slot, uint8_t life) {
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state != C_FREE && !c->lift && c->pad == pad && c->slot == slot && c->life == life) return c;
    }
    return NULL;
}

void sval_ptp_feed(const uint8_t *f, uint32_t sample_us) {
    if (f[0] != 1) return;
    uint8_t      pad = f[1] ? 1 : 0, flags = f[2] & 0x7F, mask = f[3] & 0x1F;
    uint32_t     epoch = ptp_get32(f + 12);
    pad_state_t *p     = &pads[pad];
    if (!p->present) return; // a hand without a pad never reports

    if (flags & 0x20) { // the other half's snapshot went stale
        sval_ptp_pad_lost(pad);
        return;
    }
    if (flags & 0x04) return; // invalid read: hold; SVAL_PTP_STALE_MS lifts a dead pad

    // The chip answered: whatever it said, the pad is alive.
    p->valid_us  = sample_us;
    p->has_valid = true;
    if (grabbed) {
        if (flags & 0x0B)
            grab[pad].down = false; // reset, palm or too many fingers: that finger is done
        else
            grab_feed(pad, mask, f);
    }
    if (flags & 0x08) { // chip reset or recalibration
        lift_pad(pad);
        p->known = true;
        p->up    = false;
        return;
    }
    if (flags & 0x03) { // palm or too many fingers
        palm_pad(pad);
        p->known = true;
        p->up    = false;
        return;
    }

    // 0x10 (the stream restarted) and plain frames take the slot rules below.
    p->known       = true;
    p->up          = mask == 0;
    bool new_epoch = p->has_epoch && epoch != p->epoch;
    p->epoch       = epoch;
    p->has_epoch   = true;
    if (!output) return;
    if (!p->armed) {
        // One all-up sample arms the pad. A pad lifted only because its
        // samples stopped (a late split link, a pad that missed reads) arms
        // on its next valid sample: a finger still down is reported again,
        // as a new contact, instead of staying unseen until it lifts.
        if (mask != 0 && !p->resume) return;
        p->armed  = true;
        p->resume = false;
        if (mask == 0) return;
    }
    p->resume = false;

    bool was_dirty = dirty;
    dirty          = false;
    // Contacts that left their slot, or whose slot now holds another contact.
    // After a stream restart, lifetime IDs restart too: keep a contact whose
    // slot is still occupied close to where it was.
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad || c->lift) continue;
        const uint8_t *s = f + 16 + 8 * c->slot;
        if (!(mask & (1u << c->slot))) {
            end_contact(c);
            continue;
        }
        if (new_epoch) { // lifetime IDs restarted: match by slot and position
            uint16_t x, y;
            map_xy(pad, s, &x, &y);
            if (c->state != C_IGNORED && distance(c, x, y) <= SVAL_PTP_REMAP_COUNTS)
                c->life = s[0];
            else
                end_contact(c);
        } else if (s[0] != c->life) {
            end_contact(c);
        }
    }

    for (uint8_t slot = 0; slot < 5; slot++) {
        if (!(mask & (1u << slot))) continue;
        const uint8_t *s = f + 16 + 8 * slot;
        uint16_t       x, y;
        map_xy(pad, s, &x, &y);
        // Windows' rule for a contact that isn't a finger: Confidence clear
        // in this report and every one after, until it lifts.
        const uint8_t palm_at   = sval_palm_area();
        bool          palm_area = palm_at && s[7] > palm_at;
        contact_t *c         = find_contact(pad, slot, s[0]);
        if (c) {
            if (c->state == C_IGNORED) { // never reported; tracked only for POINTING_CONTACTS
                c->x = x;
                c->y = y;
                continue;
            }
            if (c->state == C_ACTIVE) {
                uint32_t m  = (uint32_t)motion[pad] + distance(c, x, y);
                motion[pad] = m > 0xFFFF ? 0xFFFF : m;
            }
            c->x = x;
            c->y = y;
            if (palm_area) c->conf = false;
            dirty = true; // every sample with a finger down is a frame
            continue;
        }
        c = alloc_contact(pad);
        if (!c) continue; // pool full (counted): admitted on a later sample
        c->state = C_CANDIDATE;
        c->slot  = slot;
        c->life  = s[0];
        c->x     = x;
        c->y     = y;
        c->conf  = !palm_area;
        c->order = ++arrival;
        dirty    = true;
    }
    // This pad's sample goes into the next frame, stamped with its read time.
    if (dirty) {
        p->fresh     = true;
        p->sample_us = sample_us;
    }
    dirty |= was_dirty;
}

// Nothing on this pad can change until it reports something new: its last
// frame was a valid all-up, it is armed whenever reports are on, and none of
// its contacts is still down or lifting. multitouch.c then fetches the other
// half's pad only on news.
bool sval_ptp_pad_quiet(uint8_t pad) {
    const pad_state_t *p = &pads[pad & 1];
    if (!p->known || !p->up || (output && !p->armed)) return false;
    return !pad_has_contacts(pad & 1);
}

void sval_ptp_pad_lost(uint8_t pad) {
    pad &= 1;
    if (output && pad_down(pad) && lost_down[pad] != 0xFF) lost_down[pad]++;
    lift_pad(pad);
    grab[pad].down      = false;
    pads[pad].known     = false; // unknown counts as up for the mouse hold
    pads[pad].has_epoch = false;
    pads[pad].has_valid = false;
    pads[pad].fresh     = false;
    pads[pad].resume    = output; // reporting: a finger still down comes back when samples do
}

void sval_ptp_set_stale_ms(uint8_t pad, uint16_t ms) {
    pads[pad & 1].stale_ms = ms;
}

uint8_t sval_ptp_lost_down(uint8_t pad) {
    return lost_down[pad & 1];
}

// Admit waiting contacts in arrival order. A contact that finds 5 others
// already down is ignored for its whole lifetime (Microsoft's rule). One that
// only waits for a lifting contact's slot is admitted on a later frame.
static void admit(void) {
    uint8_t live = 0, slots = 0;
    for (uint8_t i = 0; i < POOL; i++) {
        if (pool[i].state != C_ACTIVE) continue;
        slots++;
        if (!pool[i].lift) live++;
    }
    for (;;) {
        contact_t *best = NULL;
        for (uint8_t i = 0; i < POOL; i++)
            if (pool[i].state == C_CANDIDATE && (!best || pool[i].order < best->order)) best = &pool[i];
        if (!best) break;
        if (live >= SVAL_PTP_MAX_CONTACTS) {
            if (best->lift) {
                free_contact(best);
            } else {
                // An ID of its own, apart from the touchpad's, so
                // POINTING_CONTACTS can show it beside the reported ones.
                best->cid   = take_xid();
                best->state = C_IGNORED;
            }
            continue;
        }
        if (slots >= SVAL_PTP_MAX_CONTACTS) break;
        best->cid   = take_cid();
        best->state = C_ACTIVE;
        slots++;
        if (!best->lift) live++;
    }
}

static void build(uint8_t *r, bool release) {
    memset(r, 0, SVAL_PTP_INPUT_SIZE);
    r[0]      = SVAL_PTP_REPORT_ID_INPUT;
    uint8_t n = 0;
    for (uint8_t i = 0; i < POOL && n < SVAL_PTP_MAX_CONTACTS; i++) {
        const contact_t *c = &pool[i];
        if (c->state != C_ACTIVE) continue;
        if (release && !c->sent_down) continue; // never reported: nothing to release
        bool     tip = release ? false : c->palm ? true : !(c->lift && c->sent_down);
        uint8_t *o   = r + 1 + 6 * n++;
        o[0]         = (c->conf ? 1 : 0) | (tip ? 2 : 0);
        o[1]         = c->cid;
        ptp_put16(o + 2, release ? c->sx : c->x);
        ptp_put16(o + 4, release ? c->sy : c->y);
    }
    r[33] = n;
    r[34] = (!release && (f_switches & 2)) ? (key_buttons & 7) : 0;
}

static void commit(bool release) {
    dirty = false;
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state != C_ACTIVE) continue;
        if (release) {
            free_contact(c);
        } else if (c->palm) {
            c->palm      = false;
            c->lift      = true;
            c->sent_down = true;
            dirty        = true;
        } else if (c->lift && c->sent_down) {
            free_contact(c);
        } else {
            if (c->lift) dirty = true; // a tap shorter than a frame: Tip = 0 next
            c->sent_down = true;
            c->sx        = c->x;
            c->sy        = c->y;
        }
    }
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_CANDIDATE) dirty = true; // waiting for a slot
    pads[0].fresh = pads[1].fresh = false;
}

// Scan time: 100 us units on this half's 1 MHz clock, wrapping at 16 bits,
// strictly increasing from one sent frame to the next. t is the newest pad
// sample in the frame (a keepalive or a release: the send time), so the host
// sees the pads' own sample spacing, not the USB half's send jitter.
static uint16_t stamp(uint32_t t) {
    if (!scan_started) {
        scan_started = true;
        scan_clock   = t;
    }
    int32_t d = (int32_t)(t - scan_clock);
    if (d > 0) {
        uint32_t acc = (uint32_t)d + scan_rem;
        scan += acc / 100;
        scan_rem   = acc % 100;
        scan_clock = t;
    }
    if (ever_sent && scan == ptp_get16(last_report + 31)) scan++;
    return scan;
}

// The newest fresh sample's time, or now when no pad has news.
static uint32_t frame_time(uint32_t now) {
    uint32_t t   = now;
    bool     any = false;
    for (uint8_t i = 0; i < 2; i++) {
        if (!pads[i].fresh) continue;
        if (!any || (int32_t)(pads[i].sample_us - t) > 0) t = pads[i].sample_us;
        any = true;
    }
    return t;
}

static void start_release(uint32_t now);

// The host stopped reading the endpoint: stop building frames. The contacts
// it saw down are released as soon as it reads again (a release never times
// out), and the pads arm again after that.
static void host_gone(uint32_t now) {
    busy_streak = false;
    bump16(&host_silent);
    start_release(now);
    clear_last_report();
}

static bool send_frame(uint32_t now, bool release) {
    uint8_t r[SVAL_PTP_INPUT_SIZE];
    if (!release) admit();
    build(r, release);
    ptp_put16(r + 31, stamp(release ? now : frame_time(now)));
    if (!ptp_try_send(r)) {
        send_busy++;
        if (!busy_streak) {
            busy_streak = true;
            busy_since  = now;
        } else if (!release && now - busy_since > SVAL_PTP_HOST_SILENT_US) {
            host_gone(now);
        }
        return false;
    }
    busy_streak = false;
    commit(release);
    last_sent = now;
    ever_sent = true;
    frames_sent++;
    sent_buttons = r[34];
    PTP_LOCK();
    memcpy(last_report, r, sizeof(r));
    PTP_UNLOCK();
    return true;
}

static void start_release(uint32_t now) {
    bool any = sent_buttons != 0;
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_ACTIVE && c->sent_down)
            any = true;
        else if (c->state != C_FREE)
            free_contact(c);
    }
    pads[0].armed = pads[1].armed = false;
    pads[0].resume = pads[1].resume = false;
    release_pending                 = any;
    release_at                      = now;
    if (!any) clear_all();
}

static bool all_up(void) {
    for (uint8_t i = 0; i < 2; i++)
        if (pads[i].present && pads[i].known && !pads[i].up) return false;
    return true;
}

bool sval_ptp_contacts_down(void) {
    if (!pad_used[0] && !pad_used[1]) return false;
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_ACTIVE || pool[i].state == C_CANDIDATE) return true;
    return false;
}

// With contacts on both pads, hold a frame that has only one pad's new sample
// until the other pad's arrives, at most SVAL_PTP_MERGE_US after the first.
static bool wait_for_other_pad(uint32_t now) {
    if (pads[0].fresh == pads[1].fresh || !pad_down(0) || !pad_down(1)) return false;
    const pad_state_t *f = pads[0].fresh ? &pads[0] : &pads[1];
    return now - f->sample_us < SVAL_PTP_MERGE_US;
}

void sval_ptp_task(void) {
    uint32_t now = ptp_now_us();
    if (f_reset) {
        // USB reset or SET_CONFIGURATION: the host starts over in mouse mode.
        PTP_LOCK();
        f_reset = false;
        PTP_UNLOCK();
        if (mode3) hold = true;
        clear_all();
        release_pending = false;
        busy_streak     = false;
        output = mode3 = false;
        sent_buttons   = 0;
        clear_last_report();
    }

    // A pad whose contacts get no valid sample for its stale time
    // (SVAL_PTP_STALE_MS, or longer for the other half's pad, see
    // sval_ptp_set_stale_ms) has stopped answering: lift them. Until then
    // they stay down where they were. The other half's pad has its own link
    // checks too, in multitouch.c.
    for (uint8_t i = 0; i < 2; i++) {
        uint32_t stale = (pads[i].stale_ms ? pads[i].stale_ms : SVAL_PTP_STALE_MS) * 1000u;
        if (pads[i].has_valid && pad_has_contacts(i) && now - pads[i].valid_us > stale) sval_ptp_pad_lost(i);
    }

    bool m3 = native && pad_mask && host_mode3();
    if (mode3 && !m3) hold = true; // the pad mouse resumes once all fingers are up
    mode3    = m3;
    bool out = m3 && !paused && !grabbed && (f_switches & 1);
    if (out != output) {
        if (out) {
            // Both pads arm again before PTP reports start. A release still
            // pending (output toggled off and on quickly) goes out first.
            if (release_pending)
                pads[0].armed = pads[1].armed = false;
            else
                clear_all();
        } else {
            start_release(now);
        }
        output = out;
    }
    if (hold && !mode3 && all_up()) hold = false;
    // A Button Switch change while reporting re-sends the buttons byte, so a
    // held button is released (switch off) or restored (switch on) at once.
    uint8_t switches = f_switches;
    if (output && ((switches ^ seen_switches) & 2) && (key_buttons || sent_buttons)) dirty = true;
    seen_switches = switches;

    bool paced = !ever_sent || now - last_sent >= SVAL_PTP_MIN_FRAME_US;
    if (release_pending) {
        // Until the host has the release, nothing else goes out; a USB reset
        // clears it.
        if (paced && send_frame(now, true)) {
            clear_all();
            release_pending = false;
        }
        return;
    }
    if (!output || !paced) return;
    bool keepalive = sval_ptp_contacts_down() && now - last_sent >= SVAL_PTP_KEEPALIVE_MS * 1000u;
    if (!dirty && !keepalive) return;
    if (!keepalive && wait_for_other_pad(now)) {
        bump16(&merge_waits);
        return;
    }
    send_frame(now, false);
}

// ------------------------------------------------------------ coordination

void sval_ptp_set_pads(uint8_t mask) {
    mask &= 3;
    for (uint8_t i = 0; i < 2; i++) {
        bool present = mask & (1u << i);
        if (pads[i].present && !present) {
            sval_ptp_pad_lost(i);
            pads[i].armed = false;
        }
        pads[i].present = present;
    }
    pad_mask = mask;
}

void sval_ptp_set_native(bool n) {
    if (native && !n && mode3) hold = true;
    native = n;
}

void sval_ptp_set_paused(bool p) {
    paused = p;
}

// Velocity Scroll holds the pads: no reports while grabbed, so the operating
// system sees every finger lift (start_release), and the pads arm again
// afterwards on an all-up sample, so a finger still down when the grab ends
// never moves the pointer.
void sval_ptp_set_grab(bool g) {
    if (g && !grabbed) {
        grab_reset(0);
        grab_reset(1);
    }
    grabbed = g;
}

bool sval_ptp_grab_ready(uint8_t pad) {
    return pads[pad & 1].present && sval_ptp_wants_frames();
}

uint16_t sval_ptp_grab_finger(uint8_t pad, uint16_t *x, uint16_t *y) {
    const grab_t *g = &grab[pad & 1];
    if (!grabbed || !g->down) return 0;
    *x = g->x;
    *y = g->y;
    return g->touch;
}

// The host asked for touchpad mode (Input Mode 3) and the touchpad mode
// setting lets it have it. With built-in gestures the host's mode is kept
// (it reads back what it wrote), but no contacts are reported: the pads stay
// the chip's mouse, and switching takes effect at once, lifting any contacts
// the host still has, as when the host leaves mode 3.
static bool host_mode3(void) {
    return f_mode == 3 && !sval_builtin_gestures();
}

bool sval_ptp_wants_frames(void) {
    return pad_mask && (native || hold);
}

bool sval_ptp_active(void) {
    return native && pad_mask && host_mode3();
}

bool sval_ptp_host_asks(void) {
    return native && pad_mask && f_mode == 3;
}

bool sval_ptp_reporting(void) {
    return output;
}

bool sval_ptp_suppress_mouse(void) {
    // mode3 covers the loop between the host leaving mode 3 and the task
    // setting hold, so the pad mouse never moves under a finger in between.
    if (!pad_mask) return false;
    return (native && (host_mode3() || mode3 || (!SVAL_PTP_MOUSE_BEFORE_MODE3 && !sval_builtin_gestures()))) || hold;
}

uint16_t sval_ptp_take_motion(uint8_t pad) {
    uint16_t m      = motion[pad & 1];
    motion[pad & 1] = 0;
    return m;
}

bool sval_ptp_key_button(uint8_t button, bool pressed) {
#if SVAL_PTP_KEY_BUTTONS
    // Pad Type 2 (external buttons): Microsoft's PTP spec puts the primary
    // (left) external button on Button 2 and the secondary (right) on
    // Button 3; Button 1 is the clickpad's integrated button. Linux
    // hid-multitouch (WIN8_PTP_BUTTONS) maps them the same way. MS_BTN3
    // (middle) has no PTP usage, so it stays on the mouse report.
    if (button > 1) return false;
    uint8_t bit = 2u << button;
    if (pressed) {
        // Only while reports go out and the host takes buttons from the pad;
        // otherwise the click would be lost, so it stays on the mouse report.
        if (!output || !(f_switches & 2)) return false;
        key_buttons |= bit;
        key_taken |= bit;
        dirty = true;
        return true;
    }
    if (!(key_taken & bit)) return false; // its press went to the mouse report
    key_taken &= ~bit;
    key_buttons &= ~bit;
    dirty = true;
    return true;
#else
    (void)button;
    (void)pressed;
    return false;
#endif
}

void sval_ptp_status(uint8_t *o) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_ACTIVE) n++;
    memset(o, 0, SVAL_PTP_STATUS_SIZE);
    o[0] = native;
    o[1] = f_mode;
    o[2] = f_latency;
    o[3] = f_switches;
    o[4] = (output ? 1 : 0) | (hold ? 2 : 0) | (pads[0].armed ? 4 : 0) | (pads[1].armed ? 8 : 0) | (release_pending ? 16 : 0) | (sval_ptp_suppress_mouse() ? 32 : 0) | (mode3 ? 64 : 0) | (paused ? 128 : 0);
    o[5] = n;
    ptp_put32(o + 6, frames_sent);
    ptp_put32(o + 10, send_busy);
    ptp_put16(o + 14, scan);
    o[16] = pad_mask;
    o[17] = f_rejected > 255 ? 255 : (uint8_t)f_rejected;
    ptp_put16(o + 18, host_silent);
    ptp_put16(o + 20, merge_waits);
    ptp_put16(o + 22, pool_full);
}

// ------------------------------------------------- contacts for host apps
// Sval POINTING_CONTACTS (ptp.h): read only, so it never changes what the
// touchpad sends. It copies the last report the computer took, so a host
// app draws the contacts the operating system has, then adds the fingers
// down beyond the touchpad's limit (bit 7), so every finger shows.

_Static_assert(SVAL_CONTACTS_REPORT_MAX >= SVAL_PTP_MAX_CONTACTS, "0x2F carries every reported contact");

// One 4-byte contact from a surface position: bits is the first byte without
// the right-pad bit. False for a position in the gap (never sent).
static bool put_contact(uint8_t *c, uint8_t bits, uint16_t x, uint16_t y) {
    uint8_t right = 0;
    if (x >= SVAL_PTP_PAD_STRIDE) {
        right = 1;
        x -= SVAL_PTP_PAD_STRIDE;
    } else if (x >= SVAL_PTP_PAD_X) {
        return false;
    }
    if (x > SVAL_PTP_PAD_X - 1) x = SVAL_PTP_PAD_X - 1;
    if (y > SVAL_PTP_PAD_Y - 1) y = SVAL_PTP_PAD_Y - 1;
    c[0]       = bits | (right ? 0x40 : 0);
    uint32_t w = (uint32_t)x | (uint32_t)y << 12;
    c[1]       = w;
    c[2]       = w >> 8;
    c[3]       = w >> 16;
    return true;
}

uint8_t sval_ptp_contacts(uint8_t *o, uint8_t length) {
    if (length < SVAL_PTP_CONTACTS_LEN) return 0;
    memset(o, 0, SVAL_PTP_CONTACTS_LEN);
    uint8_t r[SVAL_PTP_INPUT_SIZE];
    PTP_LOCK();
    memcpy(r, last_report, sizeof(r));
    PTP_UNLOCK();
    uint32_t age_ms = ever_sent ? (ptp_now_us() - last_sent) / 1000u : 0;
    o[0]            = 1;
    o[1]            = (output ? 1 : 0) | (pad_mask & 3) << 1;
    o[2]            = age_ms > 255 ? 255 : (uint8_t)age_ms;
    if (!output || !ever_sent || age_ms > SVAL_PTP_CONTACTS_STALE_MS) return SVAL_PTP_CONTACTS_LEN;
    uint8_t n     = r[33] > SVAL_PTP_MAX_CONTACTS ? SVAL_PTP_MAX_CONTACTS : r[33];
    uint8_t taken = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *s = r + 1 + 6 * i;
        if (put_contact(o + 4 + 4 * taken, (s[1] & 15) | ((s[0] & 2) ? 0x10 : 0) | ((s[0] & 1) ? 0x20 : 0), ptp_get16(s + 2), ptp_get16(s + 4))) taken++;
    }
    // Fingers down that the touchpad leaves out (more than SVAL_PTP_MAX_CONTACTS
    // at once), at their live positions, so a host app shows every finger.
    for (uint8_t i = 0; i < POOL && taken < SVAL_CONTACTS_REPORT_MAX; i++) {
        const contact_t *c = &pool[i];
        if (c->state != C_IGNORED || c->lift) continue;
        if (put_contact(o + 4 + 4 * taken, c->cid | 0x10 | (c->conf ? 0x20 : 0) | 0x80, c->x, c->y)) taken++;
    }
    o[3] = taken;
    return SVAL_PTP_CONTACTS_LEN;
}

// --------------------------------------------------------- feature reports
// These run in the USB ISR: they only read or store single bytes.

uint16_t sval_ptp_get_report(uint8_t report_type, uint8_t report_id, const uint8_t **data) {
    static uint8_t reply[2];
    if (report_type == 1) {
        if (report_id != SVAL_PTP_REPORT_ID_INPUT) return 0;
        *data = last_report;
        return SVAL_PTP_INPUT_SIZE;
    }
    if (report_type != 3) return 0;
    reply[0] = report_id;
    switch (report_id) {
        case SVAL_PTP_REPORT_ID_CAPS:
            reply[1] = SVAL_PTP_MAX_CONTACTS | (2 << 4); // pad type 2: external buttons
            break;
        case SVAL_PTP_REPORT_ID_CERT:
            *data = cert;
            return sizeof(cert);
        case SVAL_PTP_REPORT_ID_LATENCY:
            reply[1] = f_latency;
            break;
        case SVAL_PTP_REPORT_ID_MODE:
            reply[1] = f_mode;
            break;
        case SVAL_PTP_REPORT_ID_SWITCHES:
            reply[1] = f_switches;
            break;
        default:
            return 0;
    }
    *data = reply;
    return 2;
}

bool sval_ptp_set_report_allowed(uint8_t report_type, uint8_t report_id, uint16_t length) {
    return report_type == 3 && length == 2 && report_id >= SVAL_PTP_REPORT_ID_LATENCY && report_id <= SVAL_PTP_REPORT_ID_SWITCHES;
}

bool sval_ptp_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data, uint16_t length) {
    if (!sval_ptp_set_report_allowed(report_type, report_id, length) || data[0] != report_id) {
        f_rejected++;
        return false;
    }
    switch (report_id) {
        case SVAL_PTP_REPORT_ID_LATENCY:
            f_latency = data[1] & 1; // informational only
            break;
        case SVAL_PTP_REPORT_ID_MODE:
            f_mode = data[1]; // 3 touchpad, anything else mouse
            break;
        case SVAL_PTP_REPORT_ID_SWITCHES:
            f_switches = data[1] & 3;
            break;
    }
    return true;
}

void sval_ptp_usb_reset(void) {
    f_mode     = 0;
    f_latency  = 0;
    f_switches = 3;
    f_reset    = true;
}

#ifdef SVAL_PTP_HOST_TEST
// Every piece of state back to power-up, between test scenarios.
void sval_ptp_test_reset(void) {
    memset(pool, 0, sizeof(pool));
    memset(pads, 0, sizeof(pads));
    pad_used[0] = pad_used[1] = 0;
    arrival                   = 0;
    native = paused = mode3 = output = hold = release_pending = dirty = ever_sent = scan_started = busy_streak = false;
    grabbed     = false;
    grab_serial = 0;
    grab_reset(0);
    grab_reset(1);
    release_at = last_sent = scan_clock = scan_rem = frames_sent = send_busy = busy_since = 0;
    cid_busy = xid_busy = scan = host_silent = merge_waits = pool_full = 0;
    cid_next = key_buttons = sent_buttons = pad_mask = 0;
#    if SVAL_PTP_KEY_BUTTONS
    key_taken = 0;
#    endif
    seen_switches = 3;
    motion[0] = motion[1] = 0;
    lost_down[0] = lost_down[1] = 0;
    memset(last_report, 0, sizeof(last_report));
    last_report[0] = SVAL_PTP_REPORT_ID_INPUT;
    f_mode = f_latency = 0;
    f_switches         = 3;
    f_reset            = false;
    f_rejected         = 0;
}
#endif
