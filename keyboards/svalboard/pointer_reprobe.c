// A half's search for its pointing module: at boot, and a few more times in the first 30 s
// while nothing works (pointer_reprobe.h: when, and when it stops). Each search first asks
// the build's own module again (sval_pointer_retry_own), then probes the other buses one per main loop pass,
// so no pass waits on more than one bus; whatever answers is started with
// sval_pointer_select. Never runs while the half's module works.
#ifdef SVAL_REPROBE_HOST_TEST
#    include "reprobe_test_hal.h" // tests/sval_pointing: identity, clock and bus mocks
#else
#    include "quantum.h"
#    include "svalboard.h"
#endif
#include "pointing_detect.h"
#include "pointer_reprobe.h"

static bool                pointer_probed       = false; // the first search has finished
static bool                pointer_search_done  = false; // never search again
static uint16_t            pointer_searches     = 0;
static uint8_t             pointer_search_step  = 0;     // 0 between searches, else the next bus (1-3)
static uint8_t             pointer_search_skip  = 0;     // buses this search leaves out
static uint8_t             pointer_init_fails   = 0;     // modules found that didn't start
static uint32_t            pointer_search_ended = 0;     // when the last search ended (ms after boot)
static sval_probe_result_t pointer_probe        = {.detected = SVAL_SENSOR_NO_RESPONSE};

static bool sval_pointer_search_wanted(void) {
    sval_pointer_identity_t own = {0};
    sval_pointer_identity(&own);
    return sval_reprobe_wanted(own.built_for, own.detected, own.flags & SVAL_POINTER_INIT_OK, own.flags & SVAL_POINTER_AUTO, pointer_probe.detected, pointer_init_fails);
}

static void sval_pointer_search_end(void) {
    pointer_search_step  = 0;
    pointer_probed       = true;
    pointer_search_ended = timer_read32();
    pointer_searches++;
    if (!sval_pointer_search_wanted() || pointer_searches >= SVAL_REPROBE_SEARCHES) pointer_search_done = true;
}

void sval_pointer_probe_task(void) {
    static const uint8_t buses[] = {SVAL_BUS_PS2, SVAL_BUS_I2C, SVAL_BUS_SPI}; // the TrackPoint's reset (GP25) is driven last
    if (pointer_search_done) return;
    if (pointer_search_step == 0) {
        uint32_t due = sval_reprobe_due_ms(pointer_searches);
        if (!due) { // the last search has run: never again
            pointer_search_done = true;
            return;
        }
        if (timer_read32() < due) return; // only within the first 30 s, so the clock can't have wrapped
        if (pointer_searches > 0 && timer_read32() - pointer_search_ended < SVAL_REPROBE_GAP_MS) return;
        if (!sval_pointer_search_wanted()) { // its own module answered at boot, or is running
            pointer_probed      = true;
            pointer_search_done = true;
            return;
        }
        // The first search follows the driver's own start at boot; later ones try it again.
        if (pointer_searches > 0) {
            switch (sval_pointer_retry_own()) {
                case SVAL_RETRY_OK:
                    sval_pointer_search_end();
                    return;
                case SVAL_RETRY_FAILED:
                    pointer_init_fails++;
                    sval_pointer_search_end();
                    return;
                case SVAL_RETRY_STOP:
                    pointer_probed      = true;
                    pointer_search_done = true;
                    return;
                default:
                    break;
            }
        }
        pointer_search_skip = sval_pointer_own_bus();
        pointer_search_step = 1;
        return; // the buses start on the next pass
    }
    while (pointer_search_step <= sizeof(buses) && (pointer_search_skip & buses[pointer_search_step - 1])) pointer_search_step++;
    sval_probe_result_t found = {.detected = SVAL_SENSOR_NO_RESPONSE};
    if (pointer_search_step <= sizeof(buses)) found = sval_probe_bus(buses[pointer_search_step++ - 1], pointer_searches == 0);
    if (found.detected == SVAL_SENSOR_NO_RESPONSE && pointer_search_step <= sizeof(buses)) return; // the next bus on the next pass
    pointer_probe = found;
    if (found.detected != SVAL_SENSOR_NO_RESPONSE) {
        sval_pointer_select(&pointer_probe);
        sval_pointer_identity_t own = {0};
        sval_pointer_identity(&own);
        if ((own.flags & SVAL_POINTER_AUTO) && !(own.flags & SVAL_POINTER_INIT_OK)) pointer_init_fails++;
    }
    sval_pointer_search_end();
}

bool sval_pointer_searched(void) {
    return pointer_probed;
}

const sval_probe_result_t *sval_pointer_found(void) {
    return &pointer_probe;
}
