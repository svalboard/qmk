// The declarations trackpoint.c needs from drivers/ps2/ps2.h.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PS2_ACK 0xFA
extern uint8_t ps2_error;
uint8_t        ps2_host_send(uint8_t data);
uint8_t        ps2_host_recv_response(void);
uint8_t        ps2_host_recv(void);
bool           pbuf_has_data(void);
