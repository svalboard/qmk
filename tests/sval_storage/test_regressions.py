"""Host regression tests using the production C handlers.

Run: python3 -m unittest discover -s tests/sval_storage -v
Hardware dependencies are mocked; no board or flash is accessed.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fsanitize=undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


PRELUDE = '''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
'''


class StorageRegressions(unittest.TestCase):
    def test_macro_packets_and_storage_bounds(self):
        source = (ROOT / 'modules/svalboard/core/sval.c').read_text()
        start = source.index('        case sval_cmd_macro_buffer_get:')
        end = source.index('        case sval_cmd_label_get:', start)
        handler = 'bool handle(uint8_t *data, uint8_t length) { switch(data[1]) {\n' + source[start:end] + '\n} return true; }'
        run_c(
            PRELUDE + '''
#define DYNAMIC_KEYMAP_MACRO_EEPROM_SIZE 70000u
#define DYNAMIC_KEYMAP_MACRO_EEPROM_ADDR 20000u
#define sval_cmd_macro_buffer_get 31
#define sval_cmd_macro_buffer_set 32
#define sval_cmd_error 255
static uint8_t memory[90000];
static unsigned reads, writes;
uint8_t eeprom_read_byte(void *p) { uintptr_t a=(uintptr_t)p; assert(a>=20000 && a<90000); reads++; return memory[a]; }
void eeprom_update_byte(void *p, uint8_t b) { uintptr_t a=(uintptr_t)p; assert(a>=20000 && a<90000); writes++; memory[a]=b; }
uint32_t dynamic_keymap_macro_get_buffer_size(void) { return 70000; }
''' + function('quantum/nvm/eeprom/nvm_dynamic_keymap.c', 'nvm_dynamic_keymap_macro_read_buffer') + '\n' + function('quantum/nvm/eeprom/nvm_dynamic_keymap.c', 'nvm_dynamic_keymap_macro_update_buffer') + '''
void dynamic_keymap_macro_get_buffer(uint32_t a,uint16_t n,uint8_t *b) { nvm_dynamic_keymap_macro_read_buffer(a,n,b); }
void dynamic_keymap_macro_set_buffer(uint32_t a,uint16_t n,uint8_t *b) { nvm_dynamic_keymap_macro_update_buffer(a,n,b); }
''' + handler + '''
void packet(uint8_t *p,uint8_t cmd,uint32_t offset,uint8_t count) {
 memset(p,0,64); p[1]=cmd; memcpy(p+2,&offset,4); p[6]=count; p[7]=42;
}
int main(void) {
 uint8_t p[64], b[70000];
 for(unsigned cmd=31;cmd<=32;cmd++) {
  packet(p,cmd,UINT32_MAX,2); assert(handle(p,32)); assert(!reads && !writes);
  assert(cmd==31 ? p[6]==0 : p[2]==1);
  packet(p,cmd,69999,2); assert(handle(p,32)); assert(!reads && !writes);
  packet(p,cmd,0,26); assert(handle(p,32)); assert(!reads && !writes);
  packet(p,cmd,0,1); assert(!handle(p,6)); assert(!reads && !writes);
  packet(p,cmd,70000,0); assert(handle(p,32)); assert(!reads && !writes);
 }
 packet(p,32,69999,1); assert(handle(p,32)); assert(writes==1 && memory[89999]==42);
 packet(p,31,69999,1); assert(handle(p,32)); assert(reads==1 && p[7]==42);
 unsigned before=writes;
 nvm_dynamic_keymap_macro_update_buffer(UINT32_MAX,2,p); assert(writes==before);
 nvm_dynamic_keymap_macro_read_buffer(UINT32_MAX,2,p); assert(reads==1 && p[0]==0 && p[1]==0);
 memset(b,7,sizeof(b)); nvm_dynamic_keymap_macro_update_buffer(0,sizeof(b),b);
 memset(b,0,sizeof(b)); nvm_dynamic_keymap_macro_read_buffer(0,sizeof(b),b);
 assert(b[0]==7 && b[65536]==7 && b[69999]==7);
 // A wrapped Sval packet in a 64-byte report is 59 bytes from 0xDF: 52 data bytes at most.
 unsigned r0=reads;
 packet(p,31,0,53); assert(handle(p,59)); assert(reads==r0 && p[6]==0);
 packet(p,31,0,52); assert(handle(p,59)); assert(reads==r0+52 && p[6]==52 && p[7]==7 && p[58]==7);
 return 0;
}
'''
        )

    def test_reset_stores_a_usable_tap_dance_term(self):
        core = 'modules/svalboard/core/'
        header = (ROOT / core / 'sval.h').read_text()
        start = header.index('typedef struct', header.index('// Enabled when custom_tapping_term bit 15'))
        struct = header[start:header.index(';', header.index('}', start)) + 1]
        default = header[header.index('#ifndef SVAL_TAP_DANCE_DEFAULT_TERM'):]
        default = default[:default.index('#endif') + len('#endif')]
        run_c(
            PRELUDE + '#define SVAL_TAP_DANCE_ENTRIES 50\n#define SVAL_EEPROM_SIZE 2000\n#define SVAL_TAP_DANCE_OFFSET 0\n' + struct + '\n' + default + '\n' + """
static uint8_t memory[SVAL_EEPROM_SIZE];
void sval_write_eeprom(uint16_t a, const void *v, uint16_t n) { assert(a+n<=SVAL_EEPROM_SIZE); memcpy(memory+a,v,n); }
void sval_read_eeprom(uint16_t a, void *v, uint16_t n) { assert(a+n<=SVAL_EEPROM_SIZE); memcpy(v,memory+a,n); }
void dynamic_keymap_reset(void) {}
void dynamic_keymap_macro_reset(void) {}
void sval_reload_tap_dance(void) {}
void sval_reload_combo(void) {}
void sval_reload_key_override(void) {}
void sval_reload_alt_repeat_key(void) {}
void sval_reload_leader(void) {}
""" + function(core + 'sval.c', 'int sval_set_tap_dance') + '\n' + function(core + 'sval.c', 'int sval_get_tap_dance') + '\n' + function(core + 'sval.c', 'void sval_reset') + """
int main(void) {
 memset(memory, 0xAB, sizeof(memory));
 sval_reset();
 for (uint16_t i = 0; i < SVAL_TAP_DANCE_ENTRIES; i++) {
  sval_tap_dance_entry_t td;
  assert(sval_get_tap_dance(i, &td) == 0);
  assert(td.on_tap == 0 && td.on_hold == 0 && td.on_double_tap == 0 && td.on_tap_hold == 0);
  assert(td.custom_tapping_term == 200); // 200 ms, enabled bit clear
 }
 for (unsigned a = SVAL_TAP_DANCE_ENTRIES * sizeof(sval_tap_dance_entry_t); a < SVAL_EEPROM_SIZE; a++) assert(memory[a] == 0);
 return 0;
}
"""
        )

    def test_sval_reset_command_lasts_past_the_next_boot(self):
        # Sval RESET (0x0C) must leave the block marked valid, so the layout
        # written after it is not reset again on the next boot, and it resets
        # the QMK settings and reloads the labels as a first boot does.
        core = 'modules/svalboard/core/'
        source = (ROOT / core / 'sval.c').read_text()
        start = source.index('        case sval_cmd_reset:')
        end = source.index('        case sval_cmd_definition_size:', start)
        handler = 'bool handle(uint8_t *data, uint8_t length) { switch(data[1]) {\n' + source[start:end] + '\n} return true; }'
        run_c(
            PRELUDE + """
#define SVAL_EEPROM_SIZE 128
#define SVAL_MAGIC_OFFSET 100
#define SVAL_TAP_DANCE_ENTRIES 4
#define SVAL_TAP_DANCE_DEFAULT_TERM 200
#define sval_cmd_reset 0x0C
#define sval_cmd_definition_size 0x0D
typedef struct { uint16_t custom_tapping_term; } sval_tap_dance_entry_t;
static uint8_t mem[SVAL_EEPROM_SIZE];
static int keymap_resets, settings_resets, label_reloads;
static uint16_t keymap_value = 42;
void sval_write_eeprom(uint16_t a, const void *v, uint16_t n) { assert(a + n <= SVAL_EEPROM_SIZE); memcpy(mem + a, v, n); }
int sval_set_tap_dance(uint8_t i, const sval_tap_dance_entry_t *td) { return 0; }
void dynamic_keymap_reset(void) { keymap_resets++; keymap_value = 0; }
void dynamic_keymap_macro_reset(void) {}
void sval_reload_tap_dance(void) {}
void sval_reload_combo(void) {}
void sval_reload_key_override(void) {}
void sval_reload_alt_repeat_key(void) {}
void sval_reload_leader(void) {}
void sval_reload_labels(void) { label_reloads++; }
void client_wrapper_init(void) {}
void sval_qmk_settings_reset(void) { settings_resets++; }
void sval_qmk_settings_init(void) {}
static bool sval_eeprom_is_valid(void) { return mem[SVAL_MAGIC_OFFSET] == 0xA5; }
static void sval_eeprom_set_valid(void) { mem[SVAL_MAGIC_OFFSET] = 0xA5; }
""" + function(core + 'sval.c', 'void sval_reset') + '\n' + function(core + 'sval.c', 'static void sval_reset_storage') + '\n' + function(core + 'sval.c', 'void sval_init') + '\n' + handler + """
int main(void) {
 uint8_t p[32] = {0xDF, 0x0C};
 sval_init(); // first boot: blank storage
 assert(keymap_resets == 1 && settings_resets == 1 && sval_eeprom_is_valid());
 keymap_value = 7;
 int labels = label_reloads;
 assert(handle(p, sizeof(p)));
 assert(keymap_resets == 2 && keymap_value == 0);
 assert(settings_resets == 2);       // QMK settings reset with the rest
 assert(label_reloads == labels + 1); // RAM labels match the cleared storage
 assert(sval_eeprom_is_valid());
 keymap_value = 9; // the layout loaded after the reset
 sval_init();      // next boot
 assert(keymap_resets == 2 && settings_resets == 2 && keymap_value == 9);
 return 0;
}
"""
        )


if __name__ == '__main__':
    unittest.main()
