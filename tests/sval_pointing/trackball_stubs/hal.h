// Host-test stand-in for ChibiOS's hal.h, only the system lock squal.c takes.
// sim_lock_depth is owned by the test, so it can see what runs under the lock.
#pragma once
extern int         sim_lock_depth;
static inline void osalSysLock(void) {
    sim_lock_depth++;
}
static inline void osalSysUnlock(void) {
    sim_lock_depth--;
}
