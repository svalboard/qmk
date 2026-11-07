// Pointing module probes; see pointing_detect.h.
#include "pointing_detect.h"

#ifdef SVAL_DETECT_HOST_TEST
// The host test supplies the pins and a simulated device.
#    include "pointing_detect_test_hal.h"
#else
#    include "quantum.h"
#    include "svalboard.h"
// Open-drain style: "release" lets the line float up through the pull-up; "low" drives it low.
static inline void pin_release(pin_t pin) { gpio_set_pin_input_high(pin); }
static inline void pin_low(pin_t pin) { gpio_set_pin_output(pin); gpio_write_pin_low(pin); }
static inline void pin_drive(pin_t pin, bool high) { gpio_set_pin_output(pin); gpio_write_pin(pin, high); }
static inline bool pin_read(pin_t pin) { return gpio_read_pin(pin); }
#    define det_wait_us(us) wait_us(us)
#endif

#ifndef SVAL_DETECT_PS2_CLOCK
#    define SVAL_DETECT_PS2_CLOCK GP24
#    define SVAL_DETECT_PS2_DATA GP23
#    define SVAL_DETECT_I2C_SDA GP16
#    define SVAL_DETECT_I2C_SCL GP17
#    define SVAL_DETECT_SPI_SCK GP26
#    define SVAL_DETECT_SPI_MOSI GP27
#    define SVAL_DETECT_SPI_MISO GP24
#    define SVAL_DETECT_SPI_CS GP25
#endif

#define AZOTEQ_ADDRESS 0x74

// Waits up to us microseconds for pin to read level.
static bool wait_for(pin_t pin, bool level, uint32_t us) {
    while (pin_read(pin) != level) {
        if (!us--) return false;
        det_wait_us(1);
    }
    return true;
}

uint8_t sval_pmw33xx_classify(uint8_t product_id, uint8_t inverse_product_id) {
    // Datasheet signatures, as in drivers/sensors/pmw3360.c and pmw3389.c.
    if (product_id == 0x42 && inverse_product_id == 0xBD) return SVAL_SENSOR_PMW3360;
    if (product_id == 0x47 && inverse_product_id == 0xB8) return SVAL_SENSOR_PMW3389;
    // Another device's answer is a pair too: Inverse_Product_ID is Product_ID inverted. Anything
    // else is no answer: a silent bus (0x00 or 0xFF) or a sensor left in a bad state, as one
    // that answered 0x22 0x22 until it was powered off. The search resets it and looks again.
    if ((uint8_t)~product_id == inverse_product_id && product_id != 0x00 && product_id != 0xFF) return SVAL_SENSOR_UNKNOWN;
    return SVAL_SENSOR_NO_RESPONSE;
}

/* ---- PS/2 (TrackPoint), after drivers/ps2/ps2_busywait.c ---- */

#define PS2_EDGE_US 120 // between clock edges; PS/2 clocks at 10-16.7 kHz

static void ps2_inhibit(void) {
    pin_low(SVAL_DETECT_PS2_CLOCK);
    pin_release(SVAL_DETECT_PS2_DATA);
}

static bool ps2_send(uint8_t byte) {
    bool parity = true;
    ps2_inhibit();
    det_wait_us(100);
    pin_low(SVAL_DETECT_PS2_DATA); // request to send (start bit)
    pin_release(SVAL_DETECT_PS2_CLOCK);
    if (!wait_for(SVAL_DETECT_PS2_CLOCK, false, 10000)) goto fail; // device starts clocking within 10 ms
    for (uint8_t i = 0; i < 9; i++) { // 8 data bits, then odd parity
        bool bit = i < 8 ? (byte >> i) & 1 : parity;
        if (i < 8 && bit) parity = !parity;
        det_wait_us(15);
        if (bit) pin_release(SVAL_DETECT_PS2_DATA); else pin_low(SVAL_DETECT_PS2_DATA);
        if (!wait_for(SVAL_DETECT_PS2_CLOCK, true, PS2_EDGE_US) || !wait_for(SVAL_DETECT_PS2_CLOCK, false, PS2_EDGE_US)) goto fail;
    }
    det_wait_us(15);
    pin_release(SVAL_DETECT_PS2_DATA); // stop bit
    if (!wait_for(SVAL_DETECT_PS2_DATA, false, PS2_EDGE_US)) goto fail; // device acknowledges
    if (!wait_for(SVAL_DETECT_PS2_CLOCK, false, PS2_EDGE_US)) goto fail;
    if (!wait_for(SVAL_DETECT_PS2_CLOCK, true, PS2_EDGE_US) || !wait_for(SVAL_DETECT_PS2_DATA, true, PS2_EDGE_US)) goto fail;
    return true;
fail:
    ps2_inhibit();
    return false;
}

static bool ps2_recv(uint8_t *byte, uint32_t timeout_us) {
    uint8_t value = 0;
    bool parity = true;
    pin_release(SVAL_DETECT_PS2_DATA);
    pin_release(SVAL_DETECT_PS2_CLOCK);
    if (!wait_for(SVAL_DETECT_PS2_CLOCK, false, timeout_us) || pin_read(SVAL_DETECT_PS2_DATA)) goto fail; // start bit is 0
    if (!wait_for(SVAL_DETECT_PS2_CLOCK, true, PS2_EDGE_US)) goto fail;
    for (uint8_t i = 0; i < 10; i++) { // 8 data bits, parity, stop
        if (!wait_for(SVAL_DETECT_PS2_CLOCK, false, PS2_EDGE_US)) goto fail;
        bool bit = pin_read(SVAL_DETECT_PS2_DATA);
        if (i < 8) {
            if (bit) { value |= 1 << i; parity = !parity; }
        } else if (i == 8) {
            if (bit != parity) goto fail;
        } else if (!bit) {
            goto fail; // stop bit is 1
        }
        if (!wait_for(SVAL_DETECT_PS2_CLOCK, true, PS2_EDGE_US)) goto fail;
    }
    ps2_inhibit();
    *byte = value;
    return true;
fail:
    ps2_inhibit();
    return false;
}

// Get Device ID (0xF2): any PS/2 pointing device ACKs (0xFA) and sends its ID (0x00 for a TrackPoint).
static bool probe_ps2(sval_probe_result_t *found, uint8_t attempts) {
    for (uint8_t attempt = 0; attempt < attempts; attempt++) {
        uint8_t ack = 0, id = 0;
        if (attempt) det_wait_us(5000);
        if (ps2_send(0xF2) && ps2_recv(&ack, 25000) && ack == 0xFA && ps2_recv(&id, 25000)) {
            found->detected = SVAL_SENSOR_TRACKPOINT;
            found->id0      = id;
            return true;
        }
    }
    return false;
}

/* ---- I2C (Azoteq IQS5xx) ---- */

#define I2C_HALF_US 5

// Releasing SCL waits while the device stretches the clock. An IQS5xx in a low-power mode NACKs the
// first addressing, then holds SCL after the next until its communication window opens, up to one
// low-power report cycle later (datasheet 8.8.2), so allow a long stretch. A bare connector never stretches.
#define I2C_STRETCH_US 300000
static bool scl_release(void) {
    pin_release(SVAL_DETECT_I2C_SCL);
    return wait_for(SVAL_DETECT_I2C_SCL, true, I2C_STRETCH_US);
}

static bool i2c_start(void) {
    pin_release(SVAL_DETECT_I2C_SDA);
    if (!scl_release()) return false;
    det_wait_us(I2C_HALF_US);
    pin_low(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
    pin_low(SVAL_DETECT_I2C_SCL);
    return true;
}

static void i2c_stop(void) {
    pin_low(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
    scl_release();
    det_wait_us(I2C_HALF_US);
    pin_release(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
}

// Sends a byte; true when the device ACKs it.
static bool i2c_write(uint8_t byte) {
    for (uint8_t i = 0; i < 8; i++) {
        if (byte & (0x80 >> i)) pin_release(SVAL_DETECT_I2C_SDA); else pin_low(SVAL_DETECT_I2C_SDA);
        det_wait_us(I2C_HALF_US);
        if (!scl_release()) return false;
        det_wait_us(I2C_HALF_US);
        pin_low(SVAL_DETECT_I2C_SCL);
    }
    pin_release(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
    if (!scl_release()) return false;
    bool ack = !pin_read(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
    pin_low(SVAL_DETECT_I2C_SCL);
    return ack;
}

static bool i2c_read(uint8_t *byte, bool ack) {
    uint8_t value = 0;
    pin_release(SVAL_DETECT_I2C_SDA);
    for (uint8_t i = 0; i < 8; i++) {
        det_wait_us(I2C_HALF_US);
        if (!scl_release()) return false;
        value = (value << 1) | pin_read(SVAL_DETECT_I2C_SDA);
        det_wait_us(I2C_HALF_US);
        pin_low(SVAL_DETECT_I2C_SCL);
    }
    if (ack) pin_low(SVAL_DETECT_I2C_SDA);
    det_wait_us(I2C_HALF_US);
    if (!scl_release()) return false;
    det_wait_us(I2C_HALF_US);
    pin_low(SVAL_DETECT_I2C_SCL);
    pin_release(SVAL_DETECT_I2C_SDA);
    *byte = value;
    return true;
}

// Reads the IQS5xx product number (register 0x0000), then ends the comms window (write to 0xEEEE).
static bool probe_i2c(sval_probe_result_t *found) {
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        uint8_t high = 0, low = 0;
        bool ok = i2c_start() && i2c_write(AZOTEQ_ADDRESS << 1) && i2c_write(0x00) && i2c_write(0x00)
                  && i2c_start() && i2c_write((AZOTEQ_ADDRESS << 1) | 1) && i2c_read(&high, true) && i2c_read(&low, false);
        i2c_stop();
        if (ok) {
            if (i2c_start() && i2c_write(AZOTEQ_ADDRESS << 1) && i2c_write(0xEE) && i2c_write(0xEE)) i2c_write(0x01);
            i2c_stop();
            found->detected = SVAL_SENSOR_AZOTEQ;
            found->id0      = high;
            found->id1      = low;
            return true;
        }
        det_wait_us(2000);
    }
    return false;
}

/* ---- SPI mode 3 (PMW3360/PMW3389) ---- */

static uint8_t spi_read_register(uint8_t reg) {
    uint8_t value = 0;
    pin_drive(SVAL_DETECT_SPI_CS, false);
    det_wait_us(1);
    for (uint8_t i = 0; i < 8; i++) { // address, MSB first, bit 7 clear for a read
        pin_drive(SVAL_DETECT_SPI_SCK, false);
        pin_drive(SVAL_DETECT_SPI_MOSI, (reg & 0x7F) & (0x80 >> i));
        det_wait_us(1);
        pin_drive(SVAL_DETECT_SPI_SCK, true);
        det_wait_us(1);
    }
    det_wait_us(160); // tSRAD
    for (uint8_t i = 0; i < 8; i++) {
        pin_drive(SVAL_DETECT_SPI_SCK, false);
        det_wait_us(1);
        pin_drive(SVAL_DETECT_SPI_SCK, true);
        value = (value << 1) | pin_read(SVAL_DETECT_SPI_MISO);
        det_wait_us(1);
    }
    det_wait_us(1);
    pin_drive(SVAL_DETECT_SPI_CS, true);
    det_wait_us(20); // tSRR
    return value;
}

// A write: bit 7 of the address set, then the value.
static void spi_write_register(uint8_t reg, uint8_t value) {
    uint16_t word = ((uint16_t)(reg | 0x80) << 8) | value;
    pin_drive(SVAL_DETECT_SPI_CS, false);
    det_wait_us(1);
    for (uint8_t i = 0; i < 16; i++) {
        pin_drive(SVAL_DETECT_SPI_SCK, false);
        pin_drive(SVAL_DETECT_SPI_MOSI, word & (0x8000 >> i));
        det_wait_us(1);
        pin_drive(SVAL_DETECT_SPI_SCK, true);
        det_wait_us(1);
    }
    det_wait_us(35); // tSCLK-NCS for a write
    pin_drive(SVAL_DETECT_SPI_CS, true);
    det_wait_us(145); // tSWW
}

static bool probe_spi(sval_probe_result_t *found) {
    pin_drive(SVAL_DETECT_SPI_SCK, true);
    pin_drive(SVAL_DETECT_SPI_CS, true);
    pin_release(SVAL_DETECT_SPI_MISO);
    det_wait_us(10);
    pin_drive(SVAL_DETECT_SPI_CS, false); // a CS pulse resets the sensor's serial port
    det_wait_us(10);
    pin_drive(SVAL_DETECT_SPI_CS, true);
    det_wait_us(10);
    uint8_t product = spi_read_register(0x00), inverse = spi_read_register(0x3F);
    uint8_t sensor = sval_pmw33xx_classify(product, inverse);
    if (sensor == SVAL_SENSOR_NO_RESPONSE) {
        // A sensor that stays powered through an MCU restart or a power dip can be stuck,
        // answering 0x00, 0xFF or IDs that aren't a pair until it is reset (the CS pulse only
        // resets its serial port). The power-up sequence: CS high then low, and 0x5A to
        // Power_Up_Reset. The next search reads it again at least SVAL_REPROBE_GAP_MS later
        // (pointer_reprobe.h), after the sensor's 50 ms; nothing here waits for it.
        pin_drive(SVAL_DETECT_SPI_CS, false);
        det_wait_us(10);
        pin_drive(SVAL_DETECT_SPI_CS, true);
        det_wait_us(10);
        spi_write_register(0x3A, 0x5A);
        pin_release(SVAL_DETECT_SPI_CS);
        return false;
    }
    pin_release(SVAL_DETECT_SPI_CS);
    found->detected = sensor;
    found->id0      = product;
    found->id1      = inverse;
    return true;
}

// The PS/2 and SPI pins overlap (GP24, GP25), so they're always released together; the I2C
// pins only when the I2C probe runs.
static void release_all(bool i2c) {
    pin_release(SVAL_DETECT_PS2_CLOCK);
    pin_release(SVAL_DETECT_PS2_DATA);
    if (i2c) {
        pin_release(SVAL_DETECT_I2C_SDA);
        pin_release(SVAL_DETECT_I2C_SCL);
    }
    pin_release(SVAL_DETECT_SPI_SCK);
    pin_release(SVAL_DETECT_SPI_MOSI);
    pin_release(SVAL_DETECT_SPI_CS);
}

sval_probe_result_t sval_probe_pointers(uint8_t skip_buses) {
    sval_probe_result_t found = {.detected = SVAL_SENSOR_NO_RESPONSE};
    bool                i2c   = !(skip_buses & SVAL_BUS_I2C);
    release_all(i2c); // GP25 pulled up: trackball deselected, TrackPoint reset left as its module sets it
    if (!(skip_buses & SVAL_BUS_PS2) && probe_ps2(&found, 2)) goto done;
    if (i2c && probe_i2c(&found)) goto done;
    if (!(skip_buses & SVAL_BUS_SPI)) probe_spi(&found);
done:
    release_all(i2c);
    return found;
}

sval_probe_result_t sval_probe_bus(uint8_t bus, bool patient) {
    sval_probe_result_t found = {.detected = SVAL_SENSOR_NO_RESPONSE};
    bool                i2c   = bus == SVAL_BUS_I2C;
    release_all(i2c);
    if (bus == SVAL_BUS_PS2) probe_ps2(&found, patient ? 2 : 1);
    else if (i2c) probe_i2c(&found);
    else if (bus == SVAL_BUS_SPI) probe_spi(&found);
    release_all(i2c);
    return found;
}
