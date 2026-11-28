// sval_automouse_decayed (keyboards/svalboard/automouse_decay.h): motion after
// an idle longer than the decay forgets a partial count, however long the idle
// (F28: a 16-bit time wrapped every 65.536 s and kept it).
#include <stdio.h>
#include "automouse_decay.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void) {
    const uint16_t decay = 70; // the default Activation Decay, 7 x 10 ms
    const uint32_t last  = 123456;
    // Within the decay: kept. Past it: forgotten.
    CHECK(!sval_automouse_decayed(last + 30, last, decay));
    CHECK(!sval_automouse_decayed(last + 70, last, decay));
    CHECK(sval_automouse_decayed(last + 71, last, decay));
    CHECK(sval_automouse_decayed(last + 5000, last, decay));
    // An idle of a multiple of 65.536 s plus a few ms is still an idle.
    for (uint32_t n = 1; n <= 20; n++) {
        CHECK(sval_automouse_decayed(last + n * 65536u + 30, last, decay));
        CHECK(sval_automouse_decayed(last + n * 65536u, last, decay));
    }
    // Across the 32-bit wrap of the timer itself.
    CHECK(!sval_automouse_decayed(20, 0xFFFFFFF0u, decay));
    CHECK(sval_automouse_decayed(100, 0xFFFFFFF0u, decay));
    // A decay of 0 never forgets (what 0 means on this firmware).
    CHECK(!sval_automouse_decayed(last + 3600000u, last, 0));
    if (failures) return 1;
    printf("automouse_decay OK\n");
    return 0;
}
