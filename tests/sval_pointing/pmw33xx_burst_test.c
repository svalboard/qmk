// Host test of QMK's PMW33xx burst read (drivers/sensors/pmw33xx_common.c) against a
// simulated SPI bus: the panic recovery re-enters burst mode after a Motion byte that
// looks wrong, but not every poll while the sensor rests (Config2 Rest_En, where the
// OP_MODE bits read Rest1-3 by design).
#include <stdio.h>
#include <string.h>
#include "pmw33xx_common.h"

// The bus: a transaction is spi_start, then the address byte, then data.
static struct {
    bool    selected, have_addr;
    uint8_t addr;
    uint8_t motion; // the Motion byte the burst returns
    int     burst_writes, config2_writes;
} bus;

void spi_init(void) {}
bool spi_start(pin_t pin, bool lsb_first, uint8_t mode, uint16_t divisor) {
    (void)pin, (void)lsb_first, (void)mode, (void)divisor;
    bus.selected  = true;
    bus.have_addr = false;
    return true;
}
void spi_stop(void) {
    bus.selected = false;
}
spi_status_t spi_write(uint8_t data) {
    if (!bus.have_addr) {
        bus.have_addr = true;
        bus.addr      = data;
    }
    return SPI_STATUS_SUCCESS;
}
spi_status_t spi_read(void) {
    return 0;
}
spi_status_t spi_transmit(const uint8_t *data, uint16_t length) {
    if (length == 2 && (data[0] & 0x80)) { // a register write
        uint8_t reg = data[0] & 0x7f;
        if (reg == REG_Motion_Burst) bus.burst_writes++;
        if (reg == REG_Config2) bus.config2_writes++;
    }
    return SPI_STATUS_SUCCESS;
}
spi_status_t spi_receive(uint8_t *data, uint16_t length) {
    memset(data, 0, length);
    if (bus.have_addr && bus.addr == REG_Motion_Burst && length >= 1) data[0] = bus.motion;
    return SPI_STATUS_SUCCESS;
}

// The sensor's own parts of the driver (pmw3389.c), not under test.
const uint8_t pmw33xx_firmware_signature[2] = {0x47, 0xB8};
void          pmw33xx_set_cpi(uint8_t sensor, uint16_t cpi) {
    (void)sensor, (void)cpi;
}
uint16_t pmw33xx_get_cpi(uint8_t sensor) {
    (void)sensor;
    return 0;
}

static int fails;
#define CHECK(c)                                        \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                    \
        }                                               \
    } while (0)

// n burst polls with the given Motion byte; returns the Motion_Burst writes they made.
static int polls(int n, uint8_t motion) {
    bus.motion       = motion;
    bus.burst_writes = 0;
    for (int i = 0; i < n; i++) {
        pmw33xx_read_burst(0);
    }
    return bus.burst_writes;
}

#define REST_EN 0x20
#define MOT 0x80
#define OP_REST1 0x02
#define OP_REST2 0x04
#define OP_REST3 0x06
#define FRAME_RDATA 0x01

int main(void) {
    // Rest modes off (as QMK's init leaves Config2): any of bits 2:0 is a burst fault,
    // and the next poll enters burst mode again, as upstream does.
    pmw33xx_write(0, REG_Config2, 0x00);
    CHECK(polls(1, 0x00) == 1); // the first poll enters burst mode
    CHECK(polls(50, 0x00) == 0);
    CHECK(polls(50, OP_REST1) == 49); // every poll after the first enters it again

    // Rest modes on: the sensor rests with OP_MODE nonzero; the burst stays entered.
    pmw33xx_write(0, REG_Config2, REST_EN);
    CHECK(polls(1, 0x00) == 1); // the Config2 write left burst mode
    CHECK(polls(100, OP_REST1) == 0);
    CHECK(polls(100, OP_REST2) == 0);
    CHECK(polls(100, OP_REST3 | MOT) == 0);
    // Bit 0 (FRAME_RData_1st) still counts as a fault.
    CHECK(polls(10, OP_REST1 | FRAME_RDATA) == 9);

    // Off again: the OP_MODE bits count again.
    pmw33xx_write(0, REG_Config2, 0x00);
    CHECK(polls(10, OP_REST1) == 10);

    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("OK\n");
    return 0;
}
