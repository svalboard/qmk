#pragma once
#include <stdint.h>
#include "pointing_device.h"
typedef enum { AZOTEQ_IQS5XX_UNKNOWN, AZOTEQ_IQS5XX_IQS550 = 40, AZOTEQ_IQS5XX_IQS525 = 52, AZOTEQ_IQS5XX_IQS572 = 58 } azoteq_iqs5xx_product_numbers_t;
#define AZOTEQ_IQS5XX_TIMEOUT_MS 10
#define AZOTEQ_IQS5XX_REPORT_RATE 10
bool           azoteq_iqs5xx_init(void);
uint16_t       azoteq_iqs5xx_get_product(void);
void           azoteq_iqs5xx_set_cpi(uint16_t cpi);
report_mouse_t azoteq_iqs5xx_get_report(report_mouse_t mouse_report);
