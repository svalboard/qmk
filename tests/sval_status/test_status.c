// Host tests for keyboards/svalboard/status_report.c. Run by util/updater_test/run.sh.
//
// STATUS_REPORT (Sval 0x3F): page 0 packs every field at its documented
// offset; page 1 returns the build text in chunks. SV_OUTPUT_STATUS: every
// line it types uses only characters that type the same on QWERTY and QWERTZ,
// for ordinary, empty and extreme values, and the build text is cleaned.
#include "status_report.h"
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

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return le16(p) | ((uint32_t)le16(p + 2) << 16);
}

static sval_status_t sample(void) {
    sval_status_t s = {
        .flags_a        = SVAL_STATUS_A_RIGHT_SCROLL | SVAL_STATUS_A_NATURAL_SCROLL | SVAL_STATUS_A_AUTO_MOUSE | SVAL_STATUS_A_LEFT_AUTO_MOUSE | SVAL_STATUS_A_MAC,
        .flags_b        = SVAL_STATUS_B_HW_REV_B | SVAL_STATUS_B_OPTICAL_DEBOUNCE,
        .host_os        = 3,
        .left_cpi       = 800,
        .right_cpi      = 16000,
        .am_timeout_ms  = 650,
        .boost_hold     = {1, 0, 2},
        .boost_toggle   = 4,
        .boost_mult     = 5,
        .prewait_us     = 45,
        .postwait_us    = 90,
        .period_us      = 1000,
        .idle_period_ms = 10,
        .idle_after_ms  = 5000,
        .deep_period_ms = 20,
        .deep_after_s   = 300,
        .frame_us       = 100003,
        .led_us         = 123,
        .stage          = 1,
        .idle_flags     = 0x01 | 0x10,
        .deep_mhz       = 24,
        .clock_mhz      = 125,
    };
    return s;
}

static void test_pack(void) {
    sval_status_t s = sample();
    uint8_t       out[56];
    memset(out, 0xAA, sizeof out);
    CHECK(sval_status_pack(&s, out, sizeof out) == SVAL_STATUS_STATE_LEN, "length");
    CHECK(out[SVAL_STATUS_OFF_PAGE] == 0 && out[SVAL_STATUS_OFF_FORMAT] == 1, "page %u format %u", out[0], out[1]);
    CHECK(out[SVAL_STATUS_OFF_FLAGS_A] == s.flags_a, "flags a %02x", out[2]);
    CHECK(out[SVAL_STATUS_OFF_FLAGS_B] == s.flags_b, "flags b %02x", out[3]);
    CHECK(out[SVAL_STATUS_OFF_HOST_OS] == 3, "os");
    CHECK(le16(&out[SVAL_STATUS_OFF_LEFT_CPI]) == 800 && le16(&out[SVAL_STATUS_OFF_RIGHT_CPI]) == 16000, "cpi");
    CHECK(le16(&out[SVAL_STATUS_OFF_AM_TIMEOUT]) == 650, "timeout");
    CHECK(out[11] == 1 && out[12] == 0 && out[13] == 2 && out[14] == 4 && out[15] == 5, "boost");
    CHECK(le16(&out[16]) == 45 && le16(&out[18]) == 90, "waits");
    CHECK(le16(&out[20]) == 1000 && le16(&out[22]) == 10 && le16(&out[24]) == 5000 && le16(&out[26]) == 20 && le16(&out[28]) == 300, "idle timing");
    CHECK(le32(&out[30]) == 100003 && le16(&out[34]) == 123 && out[36] == 1, "measured");
    CHECK(out[37] == 0x11 && out[38] == 24 && out[39] == 125, "power");
    for (unsigned i = SVAL_STATUS_STATE_LEN; i < sizeof out; i++)
        CHECK(out[i] == 0, "tail %u zeroed", i);

    // No timeout: the flag, and 0 in the field.
    s.am_timeout_ms = -1;
    sval_status_pack(&s, out, sizeof out);
    CHECK((out[3] & SVAL_STATUS_B_AM_NO_TIMEOUT) && le16(&out[9]) == 0, "no timeout");
    // A timeout flag the caller set by mistake doesn't survive a real timeout.
    s.am_timeout_ms = 100;
    s.flags_b |= SVAL_STATUS_B_AM_NO_TIMEOUT;
    sval_status_pack(&s, out, sizeof out);
    CHECK(!(out[3] & SVAL_STATUS_B_AM_NO_TIMEOUT) && le16(&out[9]) == 100, "timeout flag follows the value");
    // Too short: nothing.
    CHECK(sval_status_pack(&s, out, SVAL_STATUS_STATE_LEN - 1) == 0, "short buffer");
    // Fits a 64-byte wrapped report: 58 bytes for [0xDF] [0x3F] and the page.
    CHECK(SVAL_STATUS_STATE_LEN <= 56, "fits a wrapped report");
}

static void test_build(void) {
    const char build[] = "svalboard/either:sval @ 0.30.3-120-gabcdef0123456789abcdef0123456789abcdef";
    uint8_t    out[56];
    char       got[128] = {0};
    size_t     total    = 0;
    for (uint8_t off = 0;;) {
        uint8_t n = sval_status_pack_build(build, off, out, sizeof out);
        CHECK(n >= 5 && out[0] == 1 && out[1] == 1, "header");
        CHECK(out[2] == strlen(build) && out[3] == off, "total %u offset %u", out[2], out[3]);
        if (out[4] == 0) break;
        CHECK(out[4] <= sizeof out - 5, "chunk fits");
        memcpy(got + total, &out[5], out[4]);
        total += out[4];
        off = (uint8_t)(off + out[4]);
    }
    CHECK(total == strlen(build) && strcmp(got, build) == 0, "reassembled: %s", got);
    // An offset past the end: no bytes.
    sval_status_pack_build(build, 200, out, sizeof out);
    CHECK(out[4] == 0, "past the end");
}

static int count_lines(const sval_status_t *s, const char *build, char *all, size_t n) {
    char line[192];
    int  lines = 0;
    all[0]     = '\0';
    for (uint8_t i = 0; sval_status_text_line(s, build, i, line, sizeof line); i++) {
        size_t len = strlen(line);
        CHECK(len < sizeof line - 1, "line %u fits: %s", i, line);
        if (!len) continue;
        CHECK(line[len - 1] == '\n', "line %u ends with Enter", i);
        for (size_t c = 0; c < len; c++)
            CHECK(sval_status_safe_char(line[c]), "line %u has '%c' (0x%02x): %s", i, line[c], (unsigned char)line[c], line);
        lines++;
        strncat(all, line, n - strlen(all) - 1);
    }
    return lines;
}

static void test_text(void) {
    static char   all[2048];
    sval_status_t s       = sample();
    const char    build[] = "svalboard/either:sval @ x4.6.7-3-gdeadbeef";
    int           lines   = count_lines(&s, build, all, sizeof all);
    CHECK(lines == 9, "lines %d", lines);
    CHECK(strstr(all, "Firmware svalboard either sval x4.6.7 3 gdeadbeef\n") != NULL, "build cleaned:\n%s", all);
    CHECK(strstr(all, "Left pointer, scroll mode off, 800 CPI. Right pointer, scroll mode on, 16000 CPI.") != NULL, "pointers");
    CHECK(strstr(all, "Computer macOS, Mac mode on. Axis scroll lock off. Natural scroll on.") != NULL, "computer");
    CHECK(strstr(all, "Auto Mouse on, left on, right off, timeout 650 ms.") != NULL, "auto mouse");
    CHECK(strstr(all, "Boost 2x 3x 5x, held 1 0 2, toggled off off on, multiplier 5.") != NULL, "boost");
    CHECK(strstr(all, "Hardware revision B. Scan wait 45 us before, 90 us after. Optical debounce on.") != NULL, "scan");
    CHECK(strstr(all, "Scan period 1000 us. Light idle 10 ms, after 5000 ms. Deep idle 20 ms, after 300 s.") != NULL, "idle");
    CHECK(strstr(all, "Measured frame 100003 us, LED on 123 us, now light idle.") != NULL, "measured");
    CHECK(strstr(all, "pointer rest on, RGB dim off, CPU sleep off, low clock off at 24 M, long naps on. Clock now 125 M.") != NULL, "power");
    CHECK(strstr(all, "Settings") == NULL, "no storage notices");

    // Storage notices, no timeout, unpaced, never, every OS.
    s.flags_b |= SVAL_STATUS_B_STORAGE_RESET | SVAL_STATUS_B_WRITE_FAILED;
    s.am_timeout_ms = -1;
    s.period_us = s.idle_after_ms = s.deep_after_s = 0;
    lines                                          = count_lines(&s, build, all, sizeof all);
    CHECK(lines == 11, "lines with notices %d", lines);
    CHECK(strstr(all, "Settings were reset") && strstr(all, "Settings cannot be saved"), "notices");
    CHECK(strstr(all, "no timeout") && strstr(all, "Scan period unpaced. Light idle 10 ms, never. Deep idle 20 ms, never."), "never:\n%s", all);
    for (uint8_t os = 0; os < 8; os++) {
        s.host_os = os;
        count_lines(&s, build, all, sizeof all);
    }

    // Extremes: every number at its widest.
    memset(&s, 0xFF, sizeof s);
    s.am_timeout_ms = 2147483647;
    count_lines(&s, build, all, sizeof all);
    // A build text with every printable ASCII character: all cleaned.
    char every[96];
    for (int c = 32; c < 127; c++)
        every[c - 32] = (char)c;
    every[95] = '\0';
    count_lines(&s, every, all, sizeof all);
    // A small buffer truncates, never overflows.
    char tiny[8];
    CHECK(sval_status_text_line(&s, build, 0, tiny, sizeof tiny) && strlen(tiny) < sizeof tiny, "tiny buffer");
}

static void test_safe_set(void) {
    CHECK(!sval_status_safe_char('y') && !sval_status_safe_char('Z') && !sval_status_safe_char(':') && !sval_status_safe_char('-'), "unsafe");
    CHECK(sval_status_safe_char('a') && sval_status_safe_char('M') && sval_status_safe_char('0') && sval_status_safe_char(',') && sval_status_safe_char('.'), "safe");
}

int main(void) {
    test_pack();
    test_build();
    test_text();
    test_safe_set();
    if (failures) {
        printf("status report: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("status report: %d checks passed\n", checks);
    return 0;
}
