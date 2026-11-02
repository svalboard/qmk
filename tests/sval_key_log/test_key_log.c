// Host test for the key-event line on the debug console
// (keyboards/svalboard/key_log.c): with debug off a key event prints nothing,
// with debug on the KL: line appears, and turning debug off stops it again.
// Also DEBUG_CONSOLE (Sval 0x3E, keyboards/svalboard/debug_console.c): its
// ops, replies and refusals, and that it turns the KL: line on and off. Built
// with and without CONSOLE_ENABLE by util/updater_test/run.sh.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "quantum.h"
#include "../../keyboards/svalboard/key_log.c"
#include "../../keyboards/svalboard/debug_console.c"

bool        debug_enable;
static char console[4096];
static int  console_len;

int console_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(console + console_len, sizeof(console) - console_len, fmt, ap);
    va_end(ap);
    if (n > 0) console_len += n;
    return n;
}

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

static keyrecord_t key(uint8_t row, uint8_t col, bool pressed) {
    keyrecord_t r   = {0};
    r.event.key.row = row;
    r.event.key.col = col;
    r.event.pressed = pressed;
    r.event.time    = 1234;
    r.event.type    = 1;
    return r;
}

static void reset_console(void) {
    console[0]  = 0;
    console_len = 0;
}

static void test_debug_off_prints_nothing(void) {
    reset_console();
    debug_enable  = false;
    keyrecord_t d = key(6, 2, true), u = key(6, 2, false);
    sval_key_log(0x0004, &d); // KC_A
    sval_key_log(0x0004, &u);
    CHECK(console_len == 0);
}

static void test_debug_on_prints_kl(void) {
    reset_console();
    debug_enable  = true;
    keyrecord_t d     = key(6, 2, true);
    d.tap.interrupted = true;
    d.tap.count       = 3;
    sval_key_log(0x0004, &d);
    CHECK(strcmp(console, "KL: kc: 0x0004, col:  2, row:  6, pressed: 1, time:  1234, int: 1, count: 3\n") == 0);
}

static void test_debug_off_again_stops(void) {
    reset_console();
    debug_enable  = true;
    keyrecord_t d = key(1, 3, true);
    sval_key_log(0x0005, &d);
    int on       = console_len;
    debug_enable = false;
    sval_key_log(0x0005, &d);
    CHECK(on > 0 && console_len == on);
}

// One 0x3E request as sval.c hands it over: [op] then the rest of a 32-byte report.
static void command(uint8_t op, uint8_t reply[3]) {
    uint8_t data[30];
    memset(data, 0xAA, sizeof(data));
    data[0] = op;
    sval_debug_console_kb(data, sizeof(data));
    memcpy(reply, data, 3);
}

static void test_command(void) {
    uint8_t r[3];
    debug_enable = false; // as at power-up
    command(SVAL_DEBUG_READ, r);
#ifdef CONSOLE_ENABLE
    keyrecord_t d = key(6, 2, true);
    CHECK(r[0] == SVAL_DEBUG_READ && r[1] == SVAL_DEBUG_DONE && r[2] == 0);

    command(SVAL_DEBUG_ON, r);
    CHECK(r[0] == SVAL_DEBUG_ON && r[1] == SVAL_DEBUG_DONE && r[2] == 1 && debug_enable);
    command(SVAL_DEBUG_READ, r);
    CHECK(r[1] == SVAL_DEBUG_DONE && r[2] == 1);
    reset_console();
    sval_key_log(0x0004, &d);
    CHECK(strncmp(console, "KL: ", 4) == 0);

    command(SVAL_DEBUG_OFF, r);
    CHECK(r[0] == SVAL_DEBUG_OFF && r[1] == SVAL_DEBUG_DONE && r[2] == 0 && !debug_enable);
    reset_console();
    sval_key_log(0x0004, &d);
    CHECK(console_len == 0);

    // An unknown op changes nothing.
    command(3, r);
    CHECK(r[0] == 3 && r[1] == SVAL_DEBUG_UNKNOWN_OP && r[2] == 0 && !debug_enable);
    debug_enable = true;
    command(0x80, r);
    CHECK(r[1] == SVAL_DEBUG_UNKNOWN_OP && r[2] == 1 && debug_enable);
    debug_enable = false;
#else
    // No console in the build: refused, and debug stays as it was.
    CHECK(r[1] == SVAL_DEBUG_NO_CONSOLE && r[2] == 0);
    command(SVAL_DEBUG_ON, r);
    CHECK(r[1] == SVAL_DEBUG_NO_CONSOLE && r[2] == 0 && !debug_enable);
#endif

    // Too short for a reply: only the status, when there is room for it.
    uint8_t shorter[2] = {SVAL_DEBUG_ON, 0xAA};
    sval_debug_console_kb(shorter, 2);
    CHECK(shorter[1] == SVAL_DEBUG_TOO_SHORT && !debug_enable);
}

int main(void) {
    test_debug_off_prints_nothing();
    test_debug_on_prints_kl();
    test_debug_off_again_stops();
    test_command();
    printf("key log: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
