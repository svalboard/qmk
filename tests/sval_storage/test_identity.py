"""Host test for the board name (keyboards/svalboard/identity.c) and the
USB product string made from it (tmk_core/protocol/usb_descriptor.c).

Run: python3 -m unittest discover -s tests/sval_storage -v

identity_set_name() is compiled against a mocked save(); no board or flash is
accessed.
"""
import unittest

from test_regressions import PRELUDE, function, run_c

IDENTITY = 'keyboards/svalboard/identity.c'
USB_DESCRIPTOR = 'tmk_core/protocol/usb_descriptor.c'


class IdentityName(unittest.TestCase):
    def test_failed_save_keeps_the_old_name_and_a_retry_writes(self):
        # A name save that fails its flash read-back leaves the old name, so
        # the same name sent again is written again rather than reported OK.
        run_c(
            PRELUDE + '''
#define IDENTITY_NAME_MAX_BYTES 64
typedef enum { IDENTITY_OK, IDENTITY_INVALID, IDENTITY_UNAVAILABLE, IDENTITY_WRITE_FAILED } identity_status_t;
typedef struct { uint32_t seq; uint8_t name_len; char name[IDENTITY_NAME_MAX_BYTES]; uint32_t crc; } identity_record_t;
static identity_record_t current = {.seq = 5, .name_len = 3, .name = "old"};
static char name_z[IDENTITY_NAME_MAX_BYTES + 1] = "old";
static bool region_ok = true;
static int saves, fail_saves;
static void identity_init(void) {}
static bool save(void) { saves++; current.seq++; current.crc = 0xBAD; return fail_saves-- <= 0; }
''' + function(IDENTITY, 'static bool utf8_valid') + '\n' + function(IDENTITY, 'identity_status_t identity_set_name') + '''
int main(void) {
 fail_saves = 1;
 assert(identity_set_name("new", 3) == IDENTITY_WRITE_FAILED);
 assert(saves == 1);
 assert(current.name_len == 3 && !memcmp(current.name, "old", 3) && current.seq == 5 && current.crc == 0);
 assert(!strcmp(name_z, "old"));
 assert(identity_set_name("new", 3) == IDENTITY_OK);
 assert(saves == 2);
 assert(current.name_len == 3 && !memcmp(current.name, "new", 3));
 assert(!strcmp(name_z, "new"));
 assert(identity_set_name("new", 3) == IDENTITY_OK && saves == 2); // unchanged: no write
 // A longer name that fails, then a shorter one: no bytes of the failed name remain.
 fail_saves = 1;
 assert(identity_set_name("a much longer name", 18) == IDENTITY_WRITE_FAILED);
 assert(current.name_len == 3 && !memcmp(current.name, "new", 4) && !strcmp(name_z, "new"));
 assert(identity_set_name("ab", 2) == IDENTITY_OK);
 for (int i = 2; i < IDENTITY_NAME_MAX_BYTES; i++) assert(current.name[i] == 0);
 assert(!strcmp(name_z, "ab"));
 return 0;
}
'''
        )

    def test_name_must_be_valid_utf16_text(self):
        # The name becomes the UTF-16 USB product string, so only UTF-8 that
        # UTF-16 can carry is saved: no overlong forms, surrogates or code
        # points past U+10FFFF, all of which the byte shape alone allows.
        run_c(
            PRELUDE + function(IDENTITY, 'static bool utf8_valid') + r"""
static bool ok(const char *s) { return utf8_valid((const uint8_t *)s, (uint8_t)strlen(s)); }
int main(void) {
 assert(ok("lightly") && ok("caf\xC3\xA9") && ok("\xE2\x82\xAC") && ok("\xF0\x9F\x98\x80"));
 assert(ok("\xC2\x80") && ok("\xE0\xA0\x80") && ok("\xF0\x90\x80\x80"));   // smallest of each length
 assert(ok("\xED\x9F\xBF") && ok("\xEE\x80\x80") && ok("\xF4\x8F\xBF\xBF")); // around the surrogates; U+10FFFF
 assert(!ok("\xC0\x80") && !ok("\xC1\xBF"));                 // overlong 2-byte (C0 80 is a NUL)
 assert(!ok("\xE0\x9F\xBF") && !ok("\xF0\x8F\xBF\xBF"));     // overlong 3- and 4-byte
 assert(!ok("\xED\xA0\x80") && !ok("\xED\xBF\xBF"));         // encoded surrogates
 assert(!ok("\xF4\x90\x80\x80") && !ok("\xF5\x80\x80\x80") && !ok("\xF7\xBF\xBF\xBF")); // past U+10FFFF
 assert(!ok("\xC3") && !ok("\x80") && !ok("a\x01"));         // unchanged: cut short, stray, control
 return 0;
}
"""
        )

    def test_usb_product_string_is_valid_utf16(self):
        # A name saved before the check above (or any other runtime name) is
        # still sent as valid UTF-16: what UTF-16 cannot carry becomes U+FFFD,
        # never a lone surrogate or a NUL unit.
        run_c(
            PRELUDE + """
#define USB_PRODUCT_STRING_MAX_UNITS 126
#define DTYPE_String 3
typedef struct { uint8_t Size; uint8_t Type; } __attribute__((packed)) USB_Descriptor_Header_t;
typedef struct { USB_Descriptor_Header_t Header; uint16_t UnicodeString[]; } __attribute__((packed)) USB_Descriptor_String_t;
static const char *name;
static const char *usb_product_string(void) { return name; }
static uint8_t ProductStringRuntime[sizeof(USB_Descriptor_Header_t) + USB_PRODUCT_STRING_MAX_UNITS * 2] __attribute__((aligned(2)));
""" + function(USB_DESCRIPTOR, 'static const void* product_string_runtime') + r"""
static int units(const char *n, uint16_t *out) {
 name = n;
 uint16_t size = 0;
 const uint8_t *d = product_string_runtime(&size);
 assert(d && d[0] == size && d[1] == DTYPE_String && size % 2 == 0);
 memcpy(out, d + 2, size - 2);
 return (size - 2) / 2;
}
#define EXPECT(n, ...) do { uint16_t want[] = {__VA_ARGS__}, got[USB_PRODUCT_STRING_MAX_UNITS]; \
 int k = units(n, got); assert(k == (int)(sizeof(want) / 2)); assert(!memcmp(got, want, sizeof(want))); } while (0)
int main(void) {
 EXPECT("lightly", 'l', 'i', 'g', 'h', 't', 'l', 'y');
 EXPECT("\xF0\x9F\x98\x80", 0xD83D, 0xDE00);              // U+1F600 as a pair
 EXPECT("\xF4\x8F\xBF\xBF", 0xDBFF, 0xDFFF);              // U+10FFFF
 EXPECT("a\xC0\x80" "b", 'a', 0xFFFD, 'b');               // overlong NUL
 EXPECT("\xE0\x81\x81", 0xFFFD);                          // overlong 'A'
 EXPECT("\xED\xA0\xBD\xED\xB8\x80", 0xFFFD, 0xFFFD);      // encoded surrogates (CESU-8)
 EXPECT("\xF4\x90\x80\x80", 0xFFFD);                      // U+110000
 EXPECT("\xF7\xBF\xBF\xBF", 0xFFFD);                      // U+1FFFFF would wrap to a bad pair
 EXPECT("\xC3x", 0xFFFD, 'x');                            // unchanged: cut short
 name = ""; uint16_t size = 0; assert(!product_string_runtime(&size));
 return 0;
}
"""
        )


if __name__ == '__main__':
    unittest.main()
