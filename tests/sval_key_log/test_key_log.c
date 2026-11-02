// Host test for the key-event line on the debug console
// (keyboards/svalboard/key_log.c): with debug off a key event prints nothing,
// with debug on the KL: line appears, and turning debug off stops it again.
// Built and run by util/updater_test/run.sh.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "quantum.h"
#include "../../keyboards/svalboard/key_log.c"

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
    keyrecord_t d = key(6, 2, true);
    sval_key_log(0x0004, &d);
    CHECK(strncmp(console, "KL: kc: 0x0004, col:  2, row:  6, pressed: 1, time:  1234", 56) == 0);
    CHECK(strchr(console, '\n') == console + console_len - 1);
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

int main(void) {
    test_debug_off_prints_nothing();
    test_debug_on_prints_kl();
    test_debug_off_again_stops();
    printf("key log: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
