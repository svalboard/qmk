// Find which pointing module is attached, whatever the firmware was built for.
//
// The Svalboard's pointing modules share one connector but each uses its own pins and
// protocol: trackball (PMW3360/PMW3389, SPI on GP24-27), Azoteq trackpad (IQS5xx, I2C on
// GP16/17) and TrackPoint (PS/2 on GP23/24). When a build's own module doesn't answer,
// these probes check the others, so a host can name the right .uf2 (Sval 0x2C).
//
// The probes toggle the pins directly instead of using the chip's SPI/I2C/PIO
// peripherals, so they work in any build. They take each pin from whatever peripheral
// had it, so only run them when the build's own module didn't answer.
//
// The trackball goes first: most boards have one, a PMW3389. A trackball that answers
// ends the search before any other bus is touched. GP25 is the trackball's chip select
// and the TrackPoint's reset line, so the SPI probe also pulses a TrackPoint's reset:
// PS/2 is asked only SVAL_DETECT_TP_SETTLE_MS later, once a TrackPoint that it reset
// has finished its self-test. I2C goes last.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Buses a build's own driver already checked; the probes skip them.
#define SVAL_BUS_PS2 0x01
#define SVAL_BUS_I2C 0x02
#define SVAL_BUS_SPI 0x04

// After the SPI probe, how long a TrackPoint gets to finish the self-test the probe's chip
// select may have started, before PS/2 asks for it. The boot search's 1.5 s
// (pointer_reprobe.h) already allows for a TrackPoint's power-on self-test; this is that
// test again after a reset.
#define SVAL_DETECT_TP_SETTLE_MS 1000u

typedef struct {
    uint8_t detected; // enum sval_pointer_sensor: a module, or SVAL_SENSOR_NO_RESPONSE
    uint8_t id0;      // PMW Product_ID, Azoteq product number high byte, or PS/2 device ID
    uint8_t id1;      // PMW Inverse_Product_ID, or Azoteq product number low byte
} sval_probe_result_t;

// Probes the buses not in skip_buses, SPI then PS/2 then I2C, stopping at the first module.
// Blocking, with the SVAL_DETECT_TP_SETTLE_MS wait after a silent SPI probe: the host test
// uses it; the firmware's searches go one bus per main loop pass (pointer_reprobe.c).
// The I2C pins are left alone when I2C is skipped, so a hardware I2C driver keeps them.
sval_probe_result_t sval_probe_pointers(uint8_t skip_buses);

// Probes one bus (SVAL_BUS_*), for the runtime searches that go one bus per main loop pass.
// The boot search is patient (two PS/2 tries, for a TrackPoint still finishing its self-test);
// later ones try PS/2 once, which caps the pass at the device's 10 ms window to answer.
sval_probe_result_t sval_probe_bus(uint8_t bus, bool patient);

// Which trackball sensor a PMW Product_ID / Inverse_Product_ID pair names.
uint8_t sval_pmw33xx_classify(uint8_t product_id, uint8_t inverse_product_id);

// A runtime search's second look at the build's own module (svalboard.c), before the other
// buses: restarts its driver when it now answers. The default does nothing (NONE).
enum sval_retry_own {
    SVAL_RETRY_NONE,   // nothing answered on its own bus: search the other buses
    SVAL_RETRY_OK,     // its module answered and its driver is running
    SVAL_RETRY_FAILED, // its module answered, and its driver didn't start
    SVAL_RETRY_STOP,   // the build can't restart its module: stop searching
};
uint8_t sval_pointer_retry_own(void);

// The search (pointer_reprobe.c): run from the main loop; whether the first search has
// finished, and what the last search of the other buses found (SVAL_SENSOR_NO_RESPONSE:
// nothing).
void                       sval_pointer_probe_task(void);
bool                       sval_pointer_searched(void);
const sval_probe_result_t *sval_pointer_found(void);

// Called with the probe's result; a build with every driver in starts that module's driver
// (pointing_auto/pointing_auto.c). The default does nothing.
void sval_pointer_select(const sval_probe_result_t *found);
