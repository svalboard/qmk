// The part of keyboards/svalboard/svalboard.h that touch_stream.c uses.
#pragma once
#include <stdint.h>
uint8_t  sval_pad_gestures(void);
uint16_t sval_pad_tuning(void);
uint16_t sval_pad_scroll_in_force_mm100(void);
uint8_t  sval_pad_palm(void);
uint8_t  sval_pad_hands(void);
