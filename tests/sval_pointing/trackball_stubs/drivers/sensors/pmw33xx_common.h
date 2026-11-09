// Host-test stand-in for the PMW33xx driver API; trackball_health_test.c simulates the chip.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef SIM_PMW3360
#    define PMW33XX_CPI_STEP 100
#    define PMW33XX_CPI_MIN 100
#    define PMW33XX_CPI_MAX 12000
#else
#    define PMW33XX_CPI_STEP 50
#    define PMW33XX_CPI_MIN 50
#    define PMW33XX_CPI_MAX 16000
#endif
#define REG_Product_ID 0x00
#define REG_Motion 0x02
#ifdef SIM_PMW3360
#define REG_Config1 0x0f
#else
#define REG_Resolution_L 0x0e
#define REG_Resolution_H 0x0f
#endif
#define REG_Config2 0x10
#define REG_SROM_ID 0x2a
#define REG_Power_Up_Reset 0x3a
#define REG_Inverse_Product_ID 0x3f
#define REG_Motion_Burst 0x50
#define REG_Lift_Config 0x63

typedef struct {
    union {
        struct {
            bool    capture_from_raw_data : 1;
            uint8_t operation_mode : 2;
            bool    is_lifted : 1;
            bool    raw_data_grab_is_raw_data : 1;
            uint8_t _reserved : 2;
            bool    is_motion : 1;
        } b;
        uint8_t w;
    } motion;
    uint8_t observation;
    int16_t delta_x;
    int16_t delta_y;
} pmw33xx_report_t;

bool             pmw33xx_init(uint8_t sensor);
bool             pmw33xx_spi_start(uint8_t sensor);
uint8_t          pmw33xx_read(uint8_t sensor, uint8_t reg);
bool             pmw33xx_write(uint8_t sensor, uint8_t reg, uint8_t val);
uint16_t         pmw33xx_get_cpi(uint8_t sensor);
void             pmw33xx_set_cpi_all_sensors(uint16_t cpi);
pmw33xx_report_t pmw33xx_read_burst(uint8_t sensor);

// The burst read's quality bytes (SQUAL and the rest), when a host polls Sval 0x39.
extern uint8_t pmw33xx_burst_extra[6];
extern bool    pmw33xx_burst_has_extra;
