// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The status report's data page and its typed text (status_report.h).
#include "status_report.h"
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

uint8_t sval_status_pack(const sval_status_t *s, uint8_t *out, uint8_t len) {
    if (len < SVAL_STATUS_STATE_LEN) return 0;
    memset(out, 0, len);
    out[SVAL_STATUS_OFF_PAGE]    = SVAL_STATUS_PAGE_STATE;
    out[SVAL_STATUS_OFF_FORMAT]  = SVAL_STATUS_FORMAT;
    out[SVAL_STATUS_OFF_FLAGS_A] = s->flags_a;
    uint8_t b                    = s->flags_b & ~SVAL_STATUS_B_AM_NO_TIMEOUT;
    if (s->am_timeout_ms < 0) b |= SVAL_STATUS_B_AM_NO_TIMEOUT;
    out[SVAL_STATUS_OFF_FLAGS_B] = b;
    out[SVAL_STATUS_OFF_HOST_OS] = s->host_os;
    put16(&out[SVAL_STATUS_OFF_LEFT_CPI], s->left_cpi);
    put16(&out[SVAL_STATUS_OFF_RIGHT_CPI], s->right_cpi);
    put16(&out[SVAL_STATUS_OFF_AM_TIMEOUT], s->am_timeout_ms < 0 ? 0 : s->am_timeout_ms > 0xFFFF ? 0xFFFF : (uint16_t)s->am_timeout_ms);
    memcpy(&out[SVAL_STATUS_OFF_BOOST_HOLD], s->boost_hold, 3);
    out[SVAL_STATUS_OFF_BOOST_TG]   = s->boost_toggle & 0x07;
    out[SVAL_STATUS_OFF_BOOST_MULT] = s->boost_mult;
    put16(&out[SVAL_STATUS_OFF_PREWAIT], s->prewait_us);
    put16(&out[SVAL_STATUS_OFF_POSTWAIT], s->postwait_us);
    put16(&out[SVAL_STATUS_OFF_PERIOD], s->period_us);
    put16(&out[SVAL_STATUS_OFF_IDLE_PERIOD], s->idle_period_ms);
    put16(&out[SVAL_STATUS_OFF_IDLE_AFTER], s->idle_after_ms);
    put16(&out[SVAL_STATUS_OFF_DEEP_PERIOD], s->deep_period_ms);
    put16(&out[SVAL_STATUS_OFF_DEEP_AFTER], s->deep_after_s);
    put32(&out[SVAL_STATUS_OFF_FRAME_US], s->frame_us);
    put16(&out[SVAL_STATUS_OFF_LED_US], s->led_us);
    out[SVAL_STATUS_OFF_STAGE]      = s->stage;
    out[SVAL_STATUS_OFF_IDLE_FLAGS] = s->idle_flags;
    out[SVAL_STATUS_OFF_DEEP_MHZ]   = s->deep_mhz;
    out[SVAL_STATUS_OFF_CLOCK_MHZ]  = s->clock_mhz;
    return SVAL_STATUS_STATE_LEN;
}

uint8_t sval_status_pack_build(const char *build, uint8_t offset, uint8_t *out, uint8_t len) {
    if (len < 5) return 0;
    memset(out, 0, len);
    size_t total = strlen(build);
    if (total > 255) total = 255;
    uint8_t n = 0;
    if (offset < total) {
        size_t room = len - 5, left = total - offset;
        n           = (uint8_t)(left < room ? left : room);
        memcpy(&out[5], build + offset, n);
    }
    out[0] = SVAL_STATUS_PAGE_BUILD;
    out[1] = SVAL_STATUS_FORMAT;
    out[2] = (uint8_t)total;
    out[3] = offset;
    out[4] = n;
    return 5 + n;
}

bool sval_status_safe_char(char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return c != 'y' && c != 'z';
    if (c >= 'A' && c <= 'Z') return c != 'Y' && c != 'Z';
    return c == ' ' || c == ',' || c == '.' || c == '\n';
}

static const char *on_off(bool on) {
    return on ? "on" : "off";
}

static const char *os_name(uint8_t os) {
    switch (os) {
        case 1:
            return "Linux";
        case 2:
            return "Windows";
        case 3:
            return "macOS";
        case 4:
            return "iOS";
        default:
            return "not known";
    }
}

// "after 5000 ms" or "never"; buf holds the words.
static const char *after_or_never(char *buf, size_t n, uint16_t v, const char *unit) {
    if (v == 0) return "never";
    snprintf(buf, n, "after %u %s", v, unit);
    return buf;
}

bool sval_status_text_line(const sval_status_t *s, const char *build, uint8_t line, char *buf, size_t n) {
    if (n == 0) return false;
    buf[0]        = '\0';
    const uint8_t a = s->flags_a, b = s->flags_b;
    char          w1[24], w2[24];
    switch (line) {
        case 0: {
            // The build text, cleaned: QMK_KEYBOARD:QMK_KEYMAP @ QMK_VERSION has
            // slashes, a colon, an at sign and dashes, none of them safe.
            int len = snprintf(buf, n, "Svalboard status. Firmware ");
            if (len < 0 || (size_t)len + 2 > n) break;
            size_t at    = (size_t)len;
            bool   space = true; // collapse runs of cleaned characters into one space
            for (const char *p = build; *p && at + 2 < n; p++) {
                char c = *p;
                if (c == '\n' || !sval_status_safe_char(c)) c = ' ';
                if (c == ' ' && space) continue;
                buf[at++] = c;
                space     = c == ' ';
            }
            while (at > 0 && buf[at - 1] == ' ') at--;
            buf[at++] = '\n';
            buf[at]   = '\0';
            break;
        }
        case 1:
            if (b & SVAL_STATUS_B_STORAGE_RESET) snprintf(buf, n, "Settings were reset because storage could not be read. Load a saved backup.\n");
            break;
        case 2:
            if (b & SVAL_STATUS_B_WRITE_FAILED) snprintf(buf, n, "Settings cannot be saved now, changes will be lost on restart. Unplug and reconnect, then check again.\n");
            break;
        case 3:
            snprintf(buf, n, "Left pointer, scroll mode %s, %u CPI. Right pointer, scroll mode %s, %u CPI.\n", on_off(a & SVAL_STATUS_A_LEFT_SCROLL), s->left_cpi, on_off(a & SVAL_STATUS_A_RIGHT_SCROLL), s->right_cpi);
            break;
        case 4:
            snprintf(buf, n, "Computer %s, Mac mode %s. Axis scroll lock %s. Natural scroll %s.\n", os_name(s->host_os), on_off(a & SVAL_STATUS_A_MAC), on_off(a & SVAL_STATUS_A_AXIS_LOCK), on_off(a & SVAL_STATUS_A_NATURAL_SCROLL));
            break;
        case 5:
            if (s->am_timeout_ms < 0)
                snprintf(w1, sizeof w1, "no timeout");
            else
                snprintf(w1, sizeof w1, "timeout %ld ms", (long)s->am_timeout_ms);
            snprintf(buf, n, "Auto Mouse %s, left %s, right %s, %s.\n", on_off(a & SVAL_STATUS_A_AUTO_MOUSE), on_off(a & SVAL_STATUS_A_LEFT_AUTO_MOUSE), on_off(a & SVAL_STATUS_A_RIGHT_AUTO_MOUSE), w1);
            break;
        case 6:
            snprintf(buf, n, "Boost 2x 3x 5x, held %u %u %u, toggled %s %s %s, multiplier %u.\n", s->boost_hold[0], s->boost_hold[1], s->boost_hold[2], on_off(s->boost_toggle & 1), on_off(s->boost_toggle & 2), on_off(s->boost_toggle & 4), s->boost_mult);
            break;
        case 7:
            snprintf(buf, n, "Hardware revision %s. Scan wait %u us before, %u us after. Optical debounce %s.\n", (b & SVAL_STATUS_B_HW_REV_B) ? "B" : "A", s->prewait_us, s->postwait_us, on_off(b & SVAL_STATUS_B_OPTICAL_DEBOUNCE));
            break;
        case 8:
            if (s->period_us)
                snprintf(w1, sizeof w1, "%u us", s->period_us);
            else
                snprintf(w1, sizeof w1, "unpaced");
            {
                char a1[24], a2[24];
                snprintf(buf, n, "Scan period %s. Light idle %u ms, %s. Deep idle %u ms, %s.\n", w1, s->idle_period_ms, after_or_never(a1, sizeof a1, s->idle_after_ms, "ms"), s->deep_period_ms, after_or_never(a2, sizeof a2, s->deep_after_s, "s"));
            }
            break;
        case 9:
            snprintf(w2, sizeof w2, "%s", s->stage == 2 ? "deep idle" : s->stage == 1 ? "light idle" : "active");
            snprintf(buf, n, "Measured frame %lu us, LED on %u us, now %s.\n", (unsigned long)s->frame_us, s->led_us, w2);
            break;
        case 10:
            // MHz has a z: the clocks are in M, for MHz.
            snprintf(buf, n, "Idle power, pointer rest %s, RGB dim %s, CPU sleep %s, low clock %s at %u M, long naps %s. Clock now %u M.\n", on_off(s->idle_flags & 0x01), on_off(s->idle_flags & 0x02), on_off(s->idle_flags & 0x04), on_off(s->idle_flags & 0x08), s->deep_mhz, on_off(s->idle_flags & 0x10), s->clock_mhz);
            break;
        default:
            return false;
    }
    return true;
}
