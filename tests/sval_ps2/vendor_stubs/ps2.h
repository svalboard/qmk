// The declarations ps2_vendor.c needs from drivers/ps2/ps2.h.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PS2_ACK 0xFA
#define PS2_ERR_NONE 0
#define PS2_ERR_STARTBIT1 1
#define PS2_ERR_STARTBIT2 2
#define PS2_ERR_PARITY 0x10
#define PS2_ERR_NODATA 0x20
extern uint8_t ps2_error;
void           ps2_host_init(void);
uint8_t        ps2_host_send(uint8_t data);
uint8_t        ps2_host_recv_response(void);
uint8_t        ps2_host_recv(void);
bool           pbuf_has_data(void);
