// Builds QMK's PMW33xx driver and trackball.c under PMW_PREFIX names, so the PMW3360
// and PMW3389 drivers (same functions, different SROM and CPI range) link side by side.
// Define PMW_PREFIX, PMW_SENSOR and the sensor's POINTING_DEVICE_* macros first.
#define SVAL_CAT2(a, b) a##b
#define SVAL_CAT(a, b) SVAL_CAT2(a, b)
#define PMW_NAME(name) SVAL_CAT(PMW_PREFIX, name)
#define pmw33xx_check_signature PMW_NAME(check_signature)
#define pmw33xx_get_cpi_wrapper PMW_NAME(get_cpi_wrapper)
#define pmw33xx_get_report PMW_NAME(get_report)
#define pmw33xx_init PMW_NAME(init)
#define pmw33xx_init_wrapper PMW_NAME(init_wrapper)
#define pmw33xx_pointing_device_driver PMW_NAME(pointing_device_driver)
#define pmw33xx_read PMW_NAME(read)
#define pmw33xx_read_burst PMW_NAME(read_burst)
#define pmw33xx_set_cpi_all_sensors PMW_NAME(set_cpi_all_sensors)
#define pmw33xx_set_cpi_wrapper PMW_NAME(set_cpi_wrapper)
#define pmw33xx_spi_start PMW_NAME(spi_start)
#define pmw33xx_srom_get_byte PMW_NAME(srom_get_byte)
#define pmw33xx_srom_get_length PMW_NAME(srom_get_length)
#define pmw33xx_upload_firmware PMW_NAME(upload_firmware)
#define pmw33xx_write PMW_NAME(write)
#define pmw33xx_burst_extra PMW_NAME(burst_extra)
#define pmw33xx_burst_has_extra PMW_NAME(burst_has_extra)
#define pmw33xx_firmware_signature PMW_NAME(firmware_signature)
#define pmw33xx_get_cpi PMW_NAME(get_cpi)
#define pmw33xx_set_cpi PMW_NAME(set_cpi)
#define pointing_device_driver_init PMW_NAME(drv_init)
#define pointing_device_driver_get_report PMW_NAME(drv_get_report)
#define pointing_device_driver_get_cpi PMW_NAME(drv_get_cpi)
#define pointing_device_driver_set_cpi PMW_NAME(drv_set_cpi)
#define sval_pointer_identity PMW_NAME(identity)
#define sval_pointer_own_bus PMW_NAME(own_bus)
#define sval_pointer_rest_apply PMW_NAME(rest_apply)
#define sval_pointer_status PMW_NAME(status)

// After the renames: headers it pulls in declare these names too.
#include "pointing_auto.h"
#include "drivers/sensors/pmw33xx_common.c"
#include PMW_SENSOR_SOURCE
#include "../trackball/trackball.c"

const sval_pointer_driver_t PMW_DRIVER = {
    .sensor      = PMW_SENSOR,
    .init        = pointing_device_driver_init,
    .get_report  = pointing_device_driver_get_report,
    .get_cpi     = pointing_device_driver_get_cpi,
    .set_cpi     = pointing_device_driver_set_cpi,
    .identity    = sval_pointer_identity,
    .rest_apply  = sval_pointer_rest_apply,
    .status      = sval_pointer_status,
};
