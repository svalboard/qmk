#pragma once
#include <stdint.h>
enum { id_custom_set_value = 0x07, id_custom_get_value = 0x08, id_custom_save = 0x09, id_unhandled = 0xFF };
enum { id_custom_channel = 0, id_qmk_backlight_channel = 1, id_qmk_rgblight_channel = 2 };
enum { id_qmk_rgblight_brightness = 1, id_qmk_rgblight_effect = 2, id_qmk_rgblight_effect_speed = 3, id_qmk_rgblight_color = 4 };
void via_qmk_rgblight_command(uint8_t *data, uint8_t length);
void via_custom_value_command_kb(uint8_t *data, uint8_t length);
