// Host test for keyboards/svalboard/storage_idle.h: when the settings store is
// consolidated early, while the board is idle (svalboard.c). Built and run by
// util/updater_test/run.sh.
#include <stdio.h>
#include "storage_idle.h"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

#define LOW 1000u
#define IDLE 10000u

static bool due(sval_storage_idle_t *st, uint32_t log_free, uint32_t host_ms, uint32_t input_ms, bool busy) {
    return sval_storage_idle_due(st, log_free, LOW, host_ms, input_ms, IDLE, busy);
}

// Only once the log is past the low mark.
static void test_log_low(void) {
    sval_storage_idle_t st = {0};
    CHECK(!due(&st, LOW, IDLE, IDLE, false));
    CHECK(!due(&st, 5000, IDLE, IDLE, false));
    CHECK(due(&st, LOW - 1, IDLE, IDLE, false));
}

// Not while a host talks (any packet, polls too) or the board is used.
static void test_idle_only(void) {
    sval_storage_idle_t st = {0};
    CHECK(!due(&st, 0, IDLE - 1, UINT32_MAX, false));
    CHECK(!due(&st, 0, UINT32_MAX, IDLE - 1, false));
    CHECK(!due(&st, 0, 0, 0, false));
    CHECK(due(&st, 0, UINT32_MAX, UINT32_MAX, false)); // no host seen yet, no input since boot
}

// Not while a key is held or the updater owns the flash.
static void test_busy(void) {
    sval_storage_idle_t st = {0};
    CHECK(!due(&st, 0, IDLE, IDLE, true));
    CHECK(due(&st, 0, IDLE, IDLE, false));
}

// Once per quiet spell: a consolidation that failed (log still low) waits for
// the board to be used, then idle again.
static void test_once_per_spell(void) {
    sval_storage_idle_t st = {0};
    CHECK(due(&st, 0, IDLE, IDLE, false));
    CHECK(!due(&st, 0, IDLE + 5000, IDLE + 5000, false));
    CHECK(!due(&st, 0, 20, IDLE + 6000, false)); // a host packet
    CHECK(!due(&st, 0, IDLE - 1, IDLE + 9000, false));
    CHECK(due(&st, 0, IDLE, IDLE + 9000, false));
}

int main(void) {
    test_log_low();
    test_idle_only();
    test_busy();
    test_once_per_spell();
    printf("storage idle: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
