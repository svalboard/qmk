// Every pointing module's bus (pointing_auto/pointing_auto.h).
#pragma once
#include_next <mcuconf.h>
#undef RP_SPI_USE_SPI1
#define RP_SPI_USE_SPI1 TRUE
#undef RP_I2C_USE_I2C0
#define RP_I2C_USE_I2C0 TRUE
