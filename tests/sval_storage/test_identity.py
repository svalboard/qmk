"""Host test for the board name (keyboards/svalboard/identity.c).

Run: python3 -m unittest discover -s tests/sval_storage -v

identity_set_name() is compiled against a mocked save(); no board or flash is
accessed.
"""
import unittest

from test_regressions import PRELUDE, function, run_c

IDENTITY = 'keyboards/svalboard/identity.c'


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


if __name__ == '__main__':
    unittest.main()
