// Simulated connector and pointing modules for the pointing_detect.c host test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pointing_detect_test_hal.h"
#include "pointing_detect.h"

enum { H_RELEASED, H_LOW, H_HIGH };
static uint8_t host[32];      // what the probe does with each pin
static bool    dev_low[32];   // a device pulling the line low
static int     dev_drive[32]; // -1 none, else a device driving the line (push-pull, MISO)
static long    now_us;
static int     cs_driven_low; // times GP25 was driven low (only the SPI probe may)

static bool line(pin_t p) {
    if (host[p] == H_LOW || dev_low[p]) return false;
    if (dev_drive[p] >= 0) return dev_drive[p];
    return true; // host high or pull-up
}

/* ---- PMW33xx on SPI: CS GP25, SCK GP26, MOSI GP27, MISO GP24 ---- */
static struct {
    bool    on;
    uint8_t pid, inv;
    bool    sel, sck;
    int     bits;
    uint8_t addr, out;
    bool    reading;
    bool    stuck;     // answers 0x00 until it gets Power_Up_Reset (0x5A)
    uint8_t garbage;   // when not 0: answers this to every read until Power_Up_Reset
    bool    writing;
    uint8_t wdata;
    int     resets;
} pmw;
static void pmw_update(void) {
    if (!pmw.on) return;
    bool sel = !line(GP25), sck = line(GP26);
    if (sel && !pmw.sel) {
        pmw.bits    = 0;
        pmw.addr    = 0;
        pmw.reading = false;
        pmw.writing = false;
    }
    if (!sel) dev_drive[GP24] = -1;
    if (sel && sck && !pmw.sck && pmw.writing) { // rising edge: data bit in
        pmw.wdata = (pmw.wdata << 1) | line(GP27);
        if (++pmw.bits == 8) {
            if ((pmw.addr & 0x7F) == 0x3A && pmw.wdata == 0x5A) {
                pmw.stuck   = false;
                pmw.garbage = 0;
                pmw.resets++;
            }
            pmw.writing = false;
            pmw.bits    = 9; // done
        }
    } else if (sel && sck && !pmw.sck && !pmw.reading && pmw.bits < 8) { // rising edge: address bit in
        pmw.addr = (pmw.addr << 1) | line(GP27);
        if (++pmw.bits == 8) {
            pmw.bits = 0;
            if (pmw.addr & 0x80) {
                pmw.writing = true;
                pmw.wdata   = 0;
            } else {
                pmw.reading = true;
                pmw.out     = pmw.stuck ? 0x00 : pmw.garbage ? pmw.garbage : (pmw.addr & 0x7F) == 0x00 ? pmw.pid : (pmw.addr & 0x7F) == 0x3F ? pmw.inv : 0x55;
            }
        }
    } else if (sel && !sck && pmw.sck && pmw.reading && pmw.bits < 8) { // falling edge: next data bit out
        dev_drive[GP24] = (pmw.out >> (7 - pmw.bits++)) & 1;
    }
    pmw.sel = sel;
    pmw.sck = sck;
}

/* ---- IQS5xx on I2C: SDA GP16, SCL GP17, address 0x74, clock stretching ---- */
enum { I_IDLE, I_ADDR, I_WRITE, I_READ };
static struct {
    bool     on;
    uint16_t product;
    int      state, bits;
    uint8_t  byte;
    bool     rw, ack_slot, master_ack_slot, sda, scl;
    uint8_t  wbuf[8];
    int      wlen;
    uint16_t reg;
    int      rindex;
    bool     ended;
    int      stretch_us;
    long     lp_cycle_us;
    bool     asleep, window;
    long     window_at;
} azo;
// lp_cycle_us > 0 models a low-power state (datasheet 8.8.2): the first addressing is NACKed and wakes
// it; the next is ACKed, then SCL is held until the next communication window, up to one cycle later.
// Ending the session (0xEEEE) lets it sleep again.
static void azo_put_bit(void) {
    dev_low[GP16] = !((azo.byte >> (7 - azo.bits)) & 1);
}
static void azo_update(void) {
    if (!azo.on) return;
    bool sda = line(GP16), scl = line(GP17);
    if (scl && azo.scl == scl && sda != azo.sda) {
        if (!sda) { // START
            azo.state     = I_ADDR;
            azo.bits      = 0;
            azo.byte      = 0;
            azo.ack_slot  = false;
            dev_low[GP16] = false;
        } else { // STOP
            if (azo.wlen >= 2 && azo.wbuf[0] == 0xEE && azo.wbuf[1] == 0xEE) {
                azo.ended  = true;
                azo.window = false;
                azo.asleep = azo.lp_cycle_us > 0;
            }
            azo.state     = I_IDLE;
            azo.wlen      = 0;
            dev_low[GP16] = false;
        }
    } else if (scl && !azo.scl) { // rising
        if (azo.state == I_READ && azo.master_ack_slot) {
            if (sda) azo.state = I_IDLE; // NACK: done
        } else if ((azo.state == I_ADDR || azo.state == I_WRITE) && !azo.ack_slot) {
            azo.byte = (azo.byte << 1) | sda;
            azo.bits++;
        }
    } else if (!scl && azo.scl) { // falling
        if (azo.state == I_ADDR || azo.state == I_WRITE) {
            if (azo.ack_slot) {
                azo.ack_slot  = false;
                dev_low[GP16] = false;
                if (azo.state == I_ADDR) {
                    if (azo.rw) {
                        azo.state  = I_READ;
                        azo.rindex = 0;
                        azo.byte   = azo.product >> 8;
                        azo.bits   = 0;
                        azo_put_bit();
                    } else {
                        azo.state = I_WRITE;
                        azo.wlen  = 0;
                    }
                }
            } else if (azo.bits == 8) {
                if (azo.state == I_ADDR) {
                    if ((azo.byte >> 1) != 0x74) {
                        azo.state = I_IDLE;
                        return;
                    }
                    if (azo.asleep) {
                        azo.asleep    = false;
                        azo.window_at = now_us + azo.lp_cycle_us;
                        azo.state     = I_IDLE;
                        return;
                    } // NACK
                    azo.rw = azo.byte & 1;
                } else if (azo.wlen < 8)
                    azo.wbuf[azo.wlen++] = azo.byte;
                dev_low[GP16]  = true;
                azo.ack_slot   = true;
                azo.bits       = 0;
                azo.byte       = 0;
                azo.stretch_us = 300; // hold SCL low a moment after each byte
                if (!azo.window) {
                    if (azo.window_at - now_us > 300) azo.stretch_us = azo.window_at - now_us;
                    azo.window = true;
                }
                dev_low[GP17] = true;
            }
        } else if (azo.state == I_READ) {
            if (azo.master_ack_slot) {
                azo.master_ack_slot = false;
                azo.byte            = azo.product & 0xFF;
                azo.bits            = 0;
                azo_put_bit();
            } else if (++azo.bits == 8) {
                dev_low[GP16]       = false;
                azo.master_ack_slot = true;
            } else
                azo_put_bit();
        }
    }
    azo.sda = line(GP16);
    azo.scl = line(GP17);
}

/* ---- TrackPoint on PS/2: CLK GP24, DATA GP23, clocked by the device in time ---- */
enum { P_IDLE, P_RECV, P_SEND_WAIT, P_SEND };
static struct {
    bool     on;
    int      state;
    long     clock_low_since;
    long     t;
    int      cycle;
    uint16_t frame;
    uint8_t  rx;
    int      rxbits;
    uint8_t  queue[4];
    int      qlen, qpos;
} ps2;
#define HALF 40
static void ps2_tick(void) {
    if (!ps2.on) return;
    bool host_clock_low = host[GP24] == H_LOW;
    switch (ps2.state) {
        case P_IDLE:
            if (host_clock_low) {
                if (ps2.clock_low_since < 0) ps2.clock_low_since = now_us;
            } else if (ps2.clock_low_since >= 0) {
                long held           = now_us - ps2.clock_low_since;
                ps2.clock_low_since = -1;
                if (held >= 90 && !line(GP23)) {
                    ps2.state  = P_RECV;
                    ps2.t      = now_us + 50;
                    ps2.cycle  = 0;
                    ps2.rx     = 0;
                    ps2.rxbits = 0;
                }
            } else if (ps2.qpos < ps2.qlen) {
                ps2.state = P_SEND_WAIT;
                ps2.t     = now_us + 300;
            }
            break;
        case P_RECV: // device clocks 11 cycles: data 0-7, parity, stop sampled on rising edges; cycle 11 is the ACK
            if (now_us < ps2.t) break;
            if (!dev_low[GP24]) {
                dev_low[GP24] = true;
                ps2.cycle++;
                if (ps2.cycle == 11) dev_low[GP23] = false;
            } else {
                dev_low[GP24] = false;
                if (ps2.cycle <= 8) ps2.rx |= line(GP23) << (ps2.cycle - 1);
                if (ps2.cycle == 10) dev_low[GP23] = true; // ACK: pull data low before the 11th clock
                if (ps2.cycle == 11) {
                    dev_low[GP23]       = false;
                    ps2.state           = P_IDLE;
                    ps2.clock_low_since = -1;
                    ps2.qlen = ps2.qpos   = 0;
                    ps2.queue[ps2.qlen++] = 0xFA;
                    if (ps2.rx == 0xF2) ps2.queue[ps2.qlen++] = 0x00;
                }
            }
            ps2.t = now_us + HALF;
            break;
        case P_SEND_WAIT: // wait for the host to stop inhibiting before the next byte
            if (host_clock_low) {
                ps2.t = now_us + 100;
                break;
            }
            if (now_us < ps2.t) break;
            {
                uint8_t b    = ps2.queue[ps2.qpos++];
                int     ones = __builtin_popcount(b);
                ps2.frame    = (uint16_t)(0 | (b << 1) | ((ones % 2 == 0) << 9) | (1 << 10));
            }
            ps2.state = P_SEND;
            ps2.cycle = 0;
            ps2.t     = now_us;
            break;
        case P_SEND: // set data while the clock is high, then pull the clock low (host samples on the falling edge)
            if (now_us < ps2.t) break;
            if (host_clock_low && !dev_low[GP24]) {
                ps2.state     = P_IDLE;
                dev_low[GP23] = false;
                break;
            } // host aborted
            if (!dev_low[GP24]) {
                if (ps2.cycle == 11) {
                    dev_low[GP23]       = false;
                    ps2.state           = P_IDLE;
                    ps2.clock_low_since = -1;
                    break;
                }
                dev_low[GP23] = !((ps2.frame >> ps2.cycle) & 1);
                dev_low[GP24] = true;
                ps2.cycle++;
            } else
                dev_low[GP24] = false;
            ps2.t = now_us + HALF;
            break;
    }
}

/* ---- HAL ---- */
static void update(void) {
    pmw_update();
    azo_update();
}
void pin_release(pin_t p) {
    host[p] = H_RELEASED;
    update();
}
void pin_low(pin_t p) {
    host[p] = H_LOW;
    update();
}
void pin_drive(pin_t p, bool high) {
    if (p == GP25 && !high) cs_driven_low++;
    host[p] = high ? H_HIGH : H_LOW;
    update();
}
bool pin_read(pin_t p) {
    return line(p);
}
void det_wait_us(uint32_t us) {
    while (us--) {
        now_us++;
        if (azo.stretch_us && !--azo.stretch_us) {
            dev_low[GP17] = false;
            azo.scl       = line(GP17);
        }
        ps2_tick();
        update();
    }
}

static void reset(void) {
    memset(host, H_RELEASED, sizeof(host));
    memset(dev_low, 0, sizeof(dev_low));
    for (int i = 0; i < 32; i++)
        dev_drive[i] = -1;
    memset(&pmw, 0, sizeof(pmw));
    memset(&azo, 0, sizeof(azo));
    memset(&ps2, 0, sizeof(ps2));
    ps2.clock_low_since = -1;
    now_us              = 0;
    cs_driven_low       = 0;
    azo.sda = azo.scl = true;
    azo.window        = true;
}

static int failures = 0;
#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); \
            failures++;                                           \
        }                                                         \
    } while (0)

static void all_released(void) {
    pin_t pins[] = {GP16, GP17, GP23, GP24, GP25, GP26, GP27};
    for (unsigned i = 0; i < sizeof(pins); i++)
        CHECK(host[pins[i]] == H_RELEASED);
}

static void nothing_attached(void) {
    reset();
    sval_probe_result_t r = sval_probe_pointers(0);
    CHECK(r.detected == SVAL_SENSOR_NO_RESPONSE);
    all_released();
    CHECK(now_us < 150000); // a bare connector costs little
}

static void trackball(uint8_t pid, uint8_t inv, uint8_t expected) {
    reset();
    pmw.on                = true;
    pmw.pid               = pid;
    pmw.inv               = inv;
    sval_probe_result_t r = sval_probe_pointers(0);
    CHECK(r.detected == expected && r.id0 == pid && r.id1 == inv);
    all_released();
}

static void trackpad(void) {
    reset();
    azo.on                = true;
    azo.product           = 40; // IQS550
    sval_probe_result_t r = sval_probe_pointers(0);
    CHECK(r.detected == SVAL_SENSOR_AZOTEQ && r.id0 == 0 && r.id1 == 40);
    CHECK(azo.ended);          // closed the comms window
    CHECK(cs_driven_low == 0); // never reached the SPI probe
    all_released();
}

// Asleep in a low-power mode with a long report cycle, as a trackpad is when nobody touches it.
static void trackpad_low_power(long cycle_us) {
    reset();
    azo.on                = true;
    azo.product           = 40;
    azo.lp_cycle_us       = cycle_us;
    azo.asleep            = true;
    azo.window            = false;
    sval_probe_result_t r = sval_probe_pointers(0);
    CHECK(r.detected == SVAL_SENSOR_AZOTEQ && r.id0 == 0 && r.id1 == 40);
    CHECK(azo.ended);
    CHECK(cs_driven_low == 0);
    all_released();
}

static void trackpoint(void) {
    reset();
    ps2.on                = true;
    sval_probe_result_t r = sval_probe_pointers(0);
    CHECK(r.detected == SVAL_SENSOR_TRACKPOINT && r.id0 == 0x00);
    CHECK(cs_driven_low == 0); // its reset line (GP25) was never driven
    all_released();
}

static void skips_own_bus(void) {
    reset();
    ps2.on = true;
    CHECK(sval_probe_pointers(SVAL_BUS_PS2).detected == SVAL_SENSOR_NO_RESPONSE);
    reset();
    azo.on      = true;
    azo.product = 58;
    CHECK(sval_probe_pointers(SVAL_BUS_I2C).detected == SVAL_SENSOR_NO_RESPONSE);
    reset();
    pmw.on  = true;
    pmw.pid = 0x47;
    pmw.inv = 0xB8;
    CHECK(sval_probe_pointers(SVAL_BUS_SPI).detected == SVAL_SENSOR_NO_RESPONSE);
    CHECK(cs_driven_low == 0);
}

// The runtime searches probe one bus per main loop pass (sval_probe_bus): what each costs
// with nothing attached, in simulated microseconds of waiting (the real loops add a little).
static long bus_cost(uint8_t bus, bool patient) {
    reset();
    long start = now_us;
    CHECK(sval_probe_bus(bus, patient).detected == SVAL_SENSOR_NO_RESPONSE);
    return now_us - start;
}

static void one_bus_costs(void) {
    long ps2_boot = bus_cost(SVAL_BUS_PS2, true), ps2 = bus_cost(SVAL_BUS_PS2, false);
    long i2c = bus_cost(SVAL_BUS_I2C, false), spi = bus_cost(SVAL_BUS_SPI, false);
    printf("bare connector, us: PS/2 %ld (boot %ld), I2C %ld, SPI %ld\n", ps2, ps2_boot, i2c, spi);
    CHECK(ps2 < 11000); // one try: the device's 10 ms window to start clocking
    CHECK(ps2_boot < 26000);
    CHECK(i2c < 8000);
    CHECK(spi < 1000);
}

static void one_bus_finds_its_module(void) {
    reset();
    ps2.on = true;
    CHECK(sval_probe_bus(SVAL_BUS_PS2, false).detected == SVAL_SENSOR_TRACKPOINT);
    CHECK(cs_driven_low == 0);
    all_released();
    reset();
    azo.on      = true;
    azo.product = 40;
    CHECK(sval_probe_bus(SVAL_BUS_I2C, false).detected == SVAL_SENSOR_AZOTEQ);
    all_released();
    reset();
    pmw.on  = true;
    pmw.pid = 0x47;
    pmw.inv = 0xB8;
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_PMW3389);
    all_released();
    // Each probes only its own bus.
    reset();
    pmw.on  = true;
    pmw.pid = 0x47;
    pmw.inv = 0xB8;
    CHECK(sval_probe_bus(SVAL_BUS_PS2, false).detected == SVAL_SENSOR_NO_RESPONSE);
    CHECK(cs_driven_low == 0);
}

// A skipped I2C bus keeps its pins: the auto builds' trackpad driver owns them.
static void skipped_i2c_pins_untouched(void) {
    reset();
    host[GP16] = H_HIGH;
    host[GP17] = H_HIGH; // stand-in for the I2C hardware's hold
    sval_probe_pointers(SVAL_BUS_I2C);
    CHECK(host[GP16] == H_HIGH && host[GP17] == H_HIGH);
    sval_probe_bus(SVAL_BUS_PS2, false);
    sval_probe_bus(SVAL_BUS_SPI, false);
    CHECK(host[GP16] == H_HIGH && host[GP17] == H_HIGH);
}

// A sensor stuck through an MCU restart (answers 0x00): the first search sends the power-up
// reset and still finds nothing, the next one finds it. A healthy one is never reset.
static void stuck_sensor(void) {
    reset();
    pmw.on    = true;
    pmw.pid   = 0x47;
    pmw.inv   = 0xB8;
    pmw.stuck = true;
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_NO_RESPONSE);
    CHECK(pmw.resets == 1);
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_PMW3389);
    CHECK(pmw.resets == 1);
    all_released();
    reset();
    pmw.on  = true;
    pmw.pid = 0x47;
    pmw.inv = 0xB8;
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_PMW3389);
    CHECK(pmw.resets == 0);
    all_released();
}

// A sensor left answering IDs that aren't a pair (the field's 0x22 0x22, from a sensor that
// stayed powered through a restart or a power dip) is no answer, not another device: the
// search resets it (CS pulse, then 0x5A to Power_Up_Reset) and the next search finds it.
static void bad_pair_sensor(uint8_t garbage) {
    reset();
    pmw.on      = true;
    pmw.pid     = 0x47;
    pmw.inv     = 0xB8;
    pmw.garbage = garbage;
    int pulses  = cs_driven_low;
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_NO_RESPONSE);
    CHECK(pmw.resets == 1);
    CHECK(cs_driven_low - pulses >= 5); // the probe's pulse, two reads, the reset's pulse, the write
    all_released();
    sval_probe_result_t r = sval_probe_bus(SVAL_BUS_SPI, false);
    CHECK(r.detected == SVAL_SENSOR_PMW3389 && r.id0 == 0x47 && r.id1 == 0xB8);
    CHECK(pmw.resets == 1);
    all_released();
}

// Another device's real answer (Inverse_Product_ID is Product_ID inverted) is never reset.
static void other_device_not_reset(void) {
    reset();
    pmw.on  = true;
    pmw.pid = 0x12;
    pmw.inv = 0xED;
    CHECK(sval_probe_bus(SVAL_BUS_SPI, false).detected == SVAL_SENSOR_UNKNOWN);
    CHECK(pmw.resets == 0);
    all_released();
}

static void classify(void) {
    CHECK(sval_pmw33xx_classify(0x42, 0xBD) == SVAL_SENSOR_PMW3360);
    CHECK(sval_pmw33xx_classify(0x47, 0xB8) == SVAL_SENSOR_PMW3389);
    CHECK(sval_pmw33xx_classify(0x00, 0x00) == SVAL_SENSOR_NO_RESPONSE);
    CHECK(sval_pmw33xx_classify(0xFF, 0xFF) == SVAL_SENSOR_NO_RESPONSE);
    CHECK(sval_pmw33xx_classify(0x12, 0xED) == SVAL_SENSOR_UNKNOWN); // a pair: another device
    CHECK(sval_pmw33xx_classify(0x12, 0x34) == SVAL_SENSOR_NO_RESPONSE);
    CHECK(sval_pmw33xx_classify(0x22, 0x22) == SVAL_SENSOR_NO_RESPONSE); // the field's stuck sensor
    CHECK(sval_pmw33xx_classify(0x47, 0x47) == SVAL_SENSOR_NO_RESPONSE);
    CHECK(sval_pmw33xx_classify(0x00, 0xFF) == SVAL_SENSOR_NO_RESPONSE); // a line that changed between reads
    CHECK(sval_pmw33xx_classify(0xFF, 0x00) == SVAL_SENSOR_NO_RESPONSE);
    CHECK(sval_pmw33xx_classify(0x47, 0xB9) == SVAL_SENSOR_NO_RESPONSE);
}

int main(void) {
    nothing_attached();
    trackball(0x42, 0xBD, SVAL_SENSOR_PMW3360);
    trackball(0x47, 0xB8, SVAL_SENSOR_PMW3389);
    trackball(0x12, 0xED, SVAL_SENSOR_UNKNOWN);
    trackpad();
    trackpad_low_power(20000);
    trackpad_low_power(160000);
    trackpad_low_power(250000);
    trackpoint();
    skips_own_bus();
    stuck_sensor();
    bad_pair_sensor(0x22);
    bad_pair_sensor(0x5A);
    other_device_not_reset();
    classify();
    one_bus_costs();
    one_bus_finds_its_module();
    skipped_i2c_pins_untouched();
    printf(failures ? "%d FAILED\n" : "OK\n", failures);
    return failures != 0;
}
