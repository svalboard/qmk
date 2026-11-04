// The split link's disconnection check (quantum/split_common/split_connection.h)
// against stalls of the other half: a 50 ms stall never disconnects, a 250 ms
// one does, whatever one failed scan costs (3 ms deadline, retries or not).
#include <assert.h>
#include <stdio.h>
#include "split_connection.h"

#define MAX_ERRORS 10
#define MIN_MS 200

// Scans from t = 0 with the other half silent for stall_ms: a good scan takes
// 1 ms, a failed one fail_ms. Returns whether the check ever fired; *after*:
// it is connected again after the stall.
static bool stall(uint16_t start, uint16_t stall_ms, uint16_t fail_ms, uint16_t min_ms, bool *after) {
    split_connection_t c     = {0};
    bool               fired = false;
    uint16_t           t     = start;
    for (int i = 0; i < 20; i++)
        t = (uint16_t)(t + 1), split_connection_update(&c, true, t, MAX_ERRORS, min_ms);
    uint16_t stall_at = t;
    while ((uint16_t)(t - stall_at) < (uint16_t)(stall_ms + 100)) {
        bool ok = (uint16_t)(t - stall_at) >= stall_ms;
        t       = (uint16_t)(t + (ok ? 1 : fail_ms));
        if (!split_connection_update(&c, ok, t, MAX_ERRORS, min_ms)) fired = true;
    }
    *after = !c.disconnected;
    return fired;
}

int main(void) {
    bool after;
    // A failed scan: one try at the 3 ms deadline, one at 20 ms, or ten tries
    // at 3 ms with transactions.c's waits between them.
    static const uint16_t costs[] = {3, 20, 34};
    for (unsigned k = 0; k < sizeof(costs) / sizeof(costs[0]); k++) {
        uint16_t fail_ms = costs[k];
        for (uint16_t start = 0; start < 2; start++) {
            uint16_t s = start ? 65500 : 0; // across the 16-bit timer's wrap
            assert(!stall(s, 50, fail_ms, MIN_MS, &after) && after);
            assert(stall(s, 250, fail_ms, MIN_MS, &after) && after);
        }
    }
    // Without the time (min 0, stock QMK): ten failed scans. Fast 3 ms
    // failures in a 50 ms stall disconnect; timeouts with retries (34 ms) do
    // not in 50 ms, and do in 400 ms.
    assert(stall(0, 50, 3, 0, &after) && after);
    assert(!stall(0, 50, 34, 0, &after) && after);
    assert(stall(0, 400, 34, 0, &after) && after);
    // Exactly at the edge: fires once the failures have gone on for 200 ms.
    split_connection_t c = {0};
    for (uint16_t t = 0; t < 199; t += 3)
        assert(split_connection_update(&c, false, t, MAX_ERRORS, MIN_MS));
    assert(!split_connection_update(&c, false, 200, MAX_ERRORS, MIN_MS));
    assert(!split_connection_update(&c, false, 203, MAX_ERRORS, MIN_MS)); // holds
    assert(split_connection_update(&c, true, 204, MAX_ERRORS, MIN_MS));   // one good scan connects
    puts("split connection: a 50 ms stall never disconnects and a 250 ms one does (failed scans of 3, 20 or 34 ms, across the timer wrap); one good scan reconnects");
    return 0;
}
