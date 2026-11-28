"""Host test for the runtime USB product string buffer
(tmk_core/protocol/usb_descriptor.c, USB_PRODUCT_STRING_RUNTIME).

Run: python3 -m unittest discover -s tests/sval_storage -v

product_string_runtime() stores the UTF-16 units through a uint16_t pointer
into ProductStringRuntime. A plain uint8_t array may be placed at any byte
address, and the RP2040's Cortex-M0+ faults on an unaligned halfword store, in
the USB interrupt that answers GET_DESCRIPTOR. The buffer must be declared at
least 2-byte aligned, so the units after the 2-byte header are too.
"""
import re
import unittest

from test_regressions import PRELUDE, ROOT, run_c

USB_DESCRIPTOR = 'tmk_core/protocol/usb_descriptor.c'


def declaration():
    source = (ROOT / USB_DESCRIPTOR).read_text()
    found = re.findall(r'^static[^;\n]*\bProductStringRuntime\[[^;\n]*;', source, flags=re.M)
    assert len(found) == 1, found
    return found[0]


class UsbProductStringAlignment(unittest.TestCase):
    def test_units_are_halfword_aligned(self):
        run_c(
            PRELUDE + '''
#define USB_PRODUCT_STRING_MAX_UNITS 126
typedef struct { uint8_t Size; uint8_t Type; } __attribute__((packed)) USB_Descriptor_Header_t;
''' + declaration() + '''
int main(void) {
 assert(__alignof__(ProductStringRuntime) >= 2);
 assert(sizeof(USB_Descriptor_Header_t) % 2 == 0);
 uintptr_t units = (uintptr_t)(ProductStringRuntime + sizeof(USB_Descriptor_Header_t));
 assert(units % 2 == 0);
 ProductStringRuntime[0] = 0;
 return ProductStringRuntime[0];
}
'''
        )


if __name__ == '__main__':
    unittest.main()
