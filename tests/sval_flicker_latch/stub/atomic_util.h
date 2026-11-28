// Mock of QMK's atomic_util.h for tests/sval_flicker_latch/test_flicker_latch.c:
// ATOMIC_BLOCK_FORCEON counts how often the kernel lock is taken and how deep
// it is held, instead of calling chSysLock()/chSysUnlock().
#pragma once
#include <stdint.h>
extern int            sval_test_lock_depth, sval_test_locks;
static inline uint8_t sval_test_lock(void) {
    sval_test_lock_depth++;
    sval_test_locks++;
    return 1;
}
static inline void sval_test_unlock(const uint8_t *s) {
    (void)s;
    sval_test_lock_depth--;
}
#define ATOMIC_BLOCK_FORCEON for (uint8_t sval_test_held __attribute__((__cleanup__(sval_test_unlock))) = sval_test_lock(), todo = 1; todo; todo = 0)
