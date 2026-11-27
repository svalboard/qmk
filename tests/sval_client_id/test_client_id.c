// Host test for the Sval client ID lease (modules/svalboard/core/client_wrapper.c)
// across the 32-bit millisecond timer's wrap. The typing pause (Sval 0x3C) is
// tied to one client ID, so an ID that never expires, or one that expires at
// once, would change when a pause can end. Built and run by util/updater_test/run.sh.
#include <stdio.h>
#include <stdlib.h>
#include "../../modules/svalboard/core/client_wrapper.c"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

static uint32_t now;
uint32_t        timer_read32(void) {
    return now;
}
void host_raw_hid_send(uint8_t *data, uint8_t length) {
    (void)data;
    (void)length;
}
bool sval_handle_command(uint8_t *data, uint8_t length) {
    (void)data;
    (void)length;
    return true;
}

#define TTL_MS ((uint32_t)CLIENT_WRAPPER_TTL_SECS * 1000)
#define STEP 0x10000u // the ID keeps the timer's high 16 bits: 65.536 s steps

// An ID allocated at `start` is valid for at least the TTL the bootstrap
// advertises, wherever it falls, including across the wrap, and expires within
// two steps after it.
static void check_from(uint32_t start) {
    now          = start;
    uint32_t id  = client_wrapper_allocate_id();
    uint32_t end = 0;
    for (uint32_t t = 0; t <= TTL_MS + 3 * STEP; t += 64) {
        now = start + t;
        if (!client_wrapper_valid_id(id)) {
            end = t;
            break;
        }
    }
    CHECK(end != 0);                 // it expires
    CHECK(end > TTL_MS);             // not before the advertised TTL
    CHECK(end <= TTL_MS + 2 * STEP); // never much past it
    // ... and stays expired for the rest of the timer's period (49.7 days),
    // sampled, until it would alias at the wrap of the age itself.
    for (uint64_t t = TTL_MS + 2 * STEP; t < 0xFFFF0000u - STEP; t += 0x01000000u) {
        now = start + (uint32_t)t;
        CHECK(!client_wrapper_valid_id(id));
    }
}

static void test_lease_from_every_phase(void) {
    for (uint32_t off = 0; off < STEP; off += 997)
        check_from(off + 12345 * STEP);
}

static void test_lease_across_wrap(void) {
    for (uint32_t back = 0; back < 2 * STEP; back += 1009)
        check_from(UINT32_MAX - back);
}

static void test_reserved_ids_never_valid(void) {
    now = 0;
    CHECK(!client_wrapper_valid_id(CLIENT_ID_BOOTSTRAP));
    CHECK(!client_wrapper_valid_id(CLIENT_ID_ERROR));
    now = UINT32_MAX;
    CHECK(!client_wrapper_valid_id(CLIENT_ID_BOOTSTRAP));
    CHECK(!client_wrapper_valid_id(CLIENT_ID_ERROR));
}

int main(void) {
    test_lease_from_every_phase();
    test_lease_across_wrap();
    test_reserved_ids_never_valid();
    printf("client id: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
