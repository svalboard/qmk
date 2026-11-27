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

    def test_macro_playback_stops_on_zero_and_bounds_nesting(self):
        core = 'modules/svalboard/core/'
        source = (ROOT / core / 'sval.c').read_text()
        send = function(core + 'sval.c', 'void dynamic_keymap_macro_send')
        # Everything from the override comment through the function, so any
        # helpers or limits the playback defines next to it come along.
        playback = source[source.index('// Override dynamic_keymap_macro_send'):source.index(send)] + send
        run_c(
            PRELUDE + r'''
#define SS_QMK_PREFIX 1
#define SS_TAP_CODE 1
#define SS_DOWN_CODE 2
#define SS_UP_CODE 3
#define SS_DELAY_CODE 4
#define VIAL_MACRO_EXT_TAP 5
#define VIAL_MACRO_EXT_DOWN 6
#define VIAL_MACRO_EXT_UP 7
#define QK_MACRO 0x7700
static uint8_t buf[64];
static char out[256];
static size_t out_len;
static unsigned depth, max_depth;
uint16_t dynamic_keymap_macro_get_count(void) { return 4; }
uint32_t dynamic_keymap_macro_get_buffer_size(void) { return sizeof(buf); }
void dynamic_keymap_macro_get_buffer(uint32_t o, uint16_t n, uint8_t *b) { assert(o + n <= sizeof(buf)); memcpy(b, buf + o, n); }
static void record(const char *s) { size_t n = strlen(s); assert(out_len + n < sizeof(out)); memcpy(out + out_len, s, n); out_len += n; out[out_len] = 0; }
void send_string(const char *s) { record(s); }
void send_string_with_delay(const char *s, uint8_t d) { record(s); }
uint16_t get_tap_code_delay(void) { return 0; }
void wait_ms(int ms) {}
void dynamic_keymap_macro_send(uint8_t id);
// A tap of a macro keycode plays that macro, as process_record_via does.
void sval_keycode_tap(uint16_t kc) {
 record("T");
 if (kc >= QK_MACRO && kc < QK_MACRO + 4) {
  if (++depth > max_depth) max_depth = depth;
  assert(depth < 20 && "macro playback recursed without limit");
  dynamic_keymap_macro_send(kc - QK_MACRO);
  depth--;
 }
}
void sval_keycode_down(uint16_t kc) { record("D"); }
void sval_keycode_up(uint16_t kc) { record("U"); }
''' + function(core + 'sval.c', 'static uint16_t decode_keycode') + '\n' + playback + r'''
static void play(const uint8_t *b, size_t n, uint8_t id) {
 memset(buf, 0, sizeof(buf)); memcpy(buf, b, n);
 out_len = 0; out[0] = 0; max_depth = 0;
 dynamic_keymap_macro_send(id);
}
int main(void) {
 // M0 taps M1 (01 05 01 77), M1 types "a": one nested macro still plays.
 const uint8_t nested[] = {1, 5, 0x01, 0x77, 0, 'a', 0};
 play(nested, sizeof(nested), 0); assert(strcmp(out, "Ta") == 0 && max_depth == 1);
 // M0 taps itself (0xFF77 is M0 with the high-byte escape): it plays once
 // more inside itself and stops there.
 const uint8_t self[] = {1, 5, 0x77, 0xFF, 0};
 play(self, sizeof(self), 0); assert(strcmp(out, "TT") == 0);
 play(self, sizeof(self), 0); assert(strcmp(out, "TT") == 0); // depth restored
 // A cycle M0 -> M1 -> M0 stops the same way.
 const uint8_t cycle[] = {1, 5, 0x01, 0x77, 0, 1, 5, 0x77, 0xFF, 0};
 play(cycle, sizeof(cycle), 0); assert(strcmp(out, "TT") == 0);
 // A zero byte inside an action ends the macro, as the start-of-macro search
 // counts it: nothing after it is typed.
 const uint8_t ext_low[] = {1, 5, 0x00, 0x52, 0, 'b', 0};
 play(ext_low, sizeof(ext_low), 0); assert(out_len == 0);
 const uint8_t ext_high[] = {1, 5, 0x04, 0x00, 'y', 0};
 play(ext_high, sizeof(ext_high), 0); assert(out_len == 0);
 const uint8_t tap0[] = {1, 1, 0x00, 'x', 0};
 play(tap0, sizeof(tap0), 0); assert(out_len == 0);
 // A whole tap action still plays, and text after it too.
 const uint8_t tap[] = {1, 1, 0x04, 'z', 0};
 play(tap, sizeof(tap), 0); assert(strcmp(out, "\x01\x01\x04z") == 0);
 return 0;
}
'''
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
