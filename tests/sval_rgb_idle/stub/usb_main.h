#pragma once
enum { USB_STOP = 1, USB_READY = 2, USB_SELECTED = 3, USB_ACTIVE = 4, USB_SUSPENDED = 5 };
typedef struct {
    int state;
} usb_driver_t;
extern usb_driver_t USB_DRIVER;
