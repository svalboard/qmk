"""Compile production read handlers and wrapper; no device or flash is accessed."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'modules/svalboard/core'


def function(source, name):
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def harness():
    source = (CORE / 'sval.c').read_text()
    header = (CORE / 'sval.h').read_text()
    declarations = '\n'.join(re.findall(r'enum \w+\s*\{.*?\};', header, re.S))
    version = re.search(r'#define SVAL_PROTOCOL_VERSION[^\n]+', header).group()
    cases = ''
    for command in ('get_info', 'layer_state_get', 'storage_reset_clear', 'default_layer_set', 'pointing_hardware'):
        start = source.index('        case sval_cmd_' + command + ':')
        end = source.index('\n        case ', start + 1)
        cases += source[start:end]
    wrapper = '\n'.join(line for line in (CORE / 'client_wrapper.c').read_text().splitlines() if not line.startswith('#include'))
    return r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "client_wrapper.h"
#ifndef REPORT
#define REPORT 64 // the raw HID report (RAW_EPSIZE, keyboards/svalboard/config.h)
#endif
#define SVAL_PREFIX 0xDF
#define SVAL_KEYBOARD_UID {1,2,3,4,5,6,7,8}
#define QMK_KEYCODES_VERSION_MAJOR 0
#define QMK_KEYCODES_VERSION_MINOR 0
#define QMK_KEYCODES_VERSION_PATCH 9
static uint32_t layer_state, default_layer_state, saved_default_layer;
static uint8_t dynamic_keymap_get_layer_count(void) { return 16; }
static void set_single_persistent_default_layer(uint8_t layer) {
 saved_default_layer = (uint32_t)1 << layer; default_layer_state = saved_default_layer;
}
static bool storage_reset;
static bool sval_storage_was_reset(void) { return storage_reset; }
static bool write_failed;
static bool sval_storage_write_failed(void) { return write_failed; }
// The keyboard's hooks (keyboards/svalboard/svalboard.c); none set here.
static uint8_t sval_storage_flags_kb(void) { return 0; }
static void sval_storage_reset_clear_kb(void) {}
static uint8_t sval_get_feature_flags3_kb(void) { return 0; }
static uint8_t sval_get_feature_flags4_kb(void) { return 0; }
#define SVAL_STORAGE_RESET_OFFSET 0
#define SVAL_STORAGE_RESET_SIZE 1
static void sval_write_eeprom(uint16_t offset, const void *data, uint16_t size) { assert(offset == 0 && size == 1); storage_reset = *(const uint8_t *)data; }
static uint8_t pointing_report_len;
static void sval_pointing_hardware_kb(uint8_t *out, uint8_t length) {
 for(int i=0;i<length;i++)assert(out[i]==0); // zeroed before the keyboard fills it
 pointing_report_len=length; out[0]=1; out[1]=0x07; out[6]=0x01;
}
static uint8_t mods_now, oneshot_now, oneshot_locked_now;
static bool caps_lock_now;
static uint8_t get_mods(void) { return mods_now; }
static uint8_t get_oneshot_mods(void) { return oneshot_now; }
static uint8_t get_oneshot_locked_mods(void) { return oneshot_locked_now; }
typedef struct { bool caps_lock; } led_t;
static led_t host_keyboard_led_state(void) { led_t led = { caps_lock_now }; return led; }
#ifdef CAPS_WORD_ENABLE
static bool caps_word_now;
static bool is_caps_word_on(void) { return caps_word_now; }
#endif
static uint8_t sent[REPORT];
static uint32_t timer_read32(void) { return 100; }
static void host_raw_hid_send(uint8_t *p,uint8_t n) { assert(n==REPORT); memcpy(sent,p,n); }
''' + declarations + '\n' + version + '\n' + function(source, 'sval_get_feature_flags') + '\n' + function(
        source, 'sval_get_feature_flags2'
    ) + '''
bool sval_handle_command(uint8_t *data,uint8_t length) {
 switch(data[1]) {
''' + cases + '''
 default: assert(false);
 } return true;
}
''' + wrapper + r'''
static uint32_t get32(uint8_t *p) {
 return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void request_with(uint8_t command, int arg) {
 uint8_t packet[REPORT]; memset(packet,0xA5,sizeof(packet));
 packet[0]=0xDD;packet[1]=1;packet[2]=packet[3]=packet[4]=0;
 packet[5]=0xDF;packet[6]=command;
 if(arg>=0)packet[7]=(uint8_t)arg;
 assert(client_wrapper_receive(packet,sizeof(packet)));
 assert(sent[0]==0xDD && get32(sent+1)==1 && sent[5]==0xDF && sent[6]==command);
}
static void request(uint8_t command) { request_with(command,-1); }
int main(void) {
 assert(sval_cmd_layer_state_get==0x16);
 assert(sval_flag_default_layer_state==0x40);
 request(sval_cmd_get_info);
 assert(get32(sent+7)==3); // No major version change for an additive capability.
 for(int i=0;i<8;i++)assert(sent[11+i]==i+1);
 assert(sent[19] & 0x40);
#ifdef CAPS_WORD_ENABLE
 assert(sent[19]==0x6F); // Context layers (bit 5) and existing flags remain independent.
#else
 assert(sent[19]==0x60); // Default-layer reporting (bit 6) plus context layers (bit 5).
#endif
 assert(sent[20]==0 && sent[21]==0 && sent[22]==9);
 assert(sent[23]==0); // Storage was not reset.
 assert(sent[24]==(sval_flag2_default_layer_set|sval_flag2_pointing_hardware|sval_flag2_modifier_state)); // Second feature byte, after the storage flags.
 storage_reset=true;
 request(sval_cmd_get_info);
 assert(sent[23]==1);
 request(sval_cmd_storage_reset_clear); // the host has told the user
 assert(!storage_reset);
 request(sval_cmd_get_info);
 assert(sent[23]==0);
 write_failed=true; // a change since boot didn't reach flash
 request(sval_cmd_get_info);
 assert(sent[23]==2);
 storage_reset=true;
 request(sval_cmd_get_info);
 assert(sent[23]==3);
 request(sval_cmd_storage_reset_clear); // clears the reset, not the live failure
 request(sval_cmd_get_info);
 assert(sent[23]==2);
 write_failed=false;
 uint32_t masks[]={0,1,2,0x80000000u,0x80000005u,0xFFFFFFFFu};
 for(unsigned a=0;a<sizeof(masks)/sizeof(masks[0]);a++) {
  for(unsigned d=0;d<sizeof(masks)/sizeof(masks[0]);d++) {
   layer_state=masks[a];default_layer_state=masks[d];
   request(sval_cmd_layer_state_get);
   assert(get32(sent+7)==masks[a]); // Existing Keybard decoder's exact offset.
   assert(get32(sent+11)==masks[d]);
   assert(layer_state==masks[a] && default_layer_state==masks[d]);
   assert(sent[15]==0 && sent[16]==0); // No modifiers held.
   for(int i=17;i<REPORT;i++)assert(sent[i]==0xA5);
  }
 }
 // Modifiers: held, one-shot pending and one-shot locked, in the HID modifier byte.
 assert(sval_flag2_modifier_state==0x04);
 mods_now=0x02; request(sval_cmd_layer_state_get); assert(sent[15]==0x02 && sent[16]==0);
 oneshot_now=0x20; request(sval_cmd_layer_state_get); assert(sent[15]==0x22);
 mods_now=0; oneshot_now=0; oneshot_locked_now=0x01; request(sval_cmd_layer_state_get); assert(sent[15]==0x01);
 oneshot_locked_now=0; caps_lock_now=true; request(sval_cmd_layer_state_get); assert(sent[15]==0 && sent[16]==sval_status_caps_lock);
 caps_lock_now=false;
#ifdef CAPS_WORD_ENABLE
 caps_word_now=true; request(sval_cmd_layer_state_get); assert(sent[16]==sval_status_caps_word);
 caps_word_now=false;
#endif
 // Ten or eleven inner bytes: the masks fit, the modifier bytes are left alone.
 mods_now=0x02;
 for(uint8_t length=10;length<12;length++) {
  uint8_t buffer[REPORT];memset(buffer,0xA5,sizeof(buffer));
  buffer[0]=0xDF;buffer[1]=0x16;
  assert(sval_handle_command(buffer,length));
  assert(buffer[1]==0x16 && buffer[10]==0xA5 && buffer[11]==0xA5);
 }
 mods_now=0;
 for(uint8_t length=2;length<10;length++) {
  uint8_t short_packet[REPORT];memset(short_packet,0xA5,sizeof(short_packet));
  short_packet[0]=0xDF;short_packet[1]=0x16;
  assert(!sval_handle_command(short_packet,length));
  assert(short_packet[1]==0xFF);
  for(int i=2;i<REPORT;i++)assert(short_packet[i]==0xA5);
 }
 // Setting the default layer saves it, like a PDF key. 0x2A stays the storage-reset clear.
 assert(sval_cmd_default_layer_set==0x2B && sval_cmd_storage_reset_clear==0x2A);
 layer_state=0x5;default_layer_state=1;saved_default_layer=1;storage_reset=true;
 request_with(sval_cmd_default_layer_set,3);
 assert(sent[7]==0 && default_layer_state==0x8 && saved_default_layer==0x8 && layer_state==0x5);
 assert(storage_reset); // Doesn't touch the storage-reset flag.
 request_with(sval_cmd_default_layer_set,15);
 assert(sent[7]==0 && saved_default_layer==0x8000);
 request_with(sval_cmd_default_layer_set,16); // No such layer: nothing changes.
 assert(sent[7]==1 && saved_default_layer==0x8000 && default_layer_state==0x8000);
 // Pointing hardware: the keyboard fills a zeroed reply after the command byte.
 assert(sval_cmd_pointing_hardware==0x2C && sval_flag2_pointing_hardware==0x02);
 request(sval_cmd_pointing_hardware);
 assert(pointing_report_len==REPORT-7 && sent[7]==1 && sent[8]==0x07 && sent[13]==0x01);
 for(int i=14;i<REPORT;i++)assert(sent[i]==0);
}
'''


class LayerReportingTests(unittest.TestCase):
    def test_wrapped_masks_capability_legacy_offsets_and_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / 'layers.c'
            source.write_text(harness())
            # 64-byte reports (Svalboard), and 32 (the QMK default) as before
            features = ['-DCAPS_WORD_ENABLE', '-DLAYER_LOCK_ENABLE', '-DONESHOT_ENABLE', '-DLEADER_ENABLE']
            for flags in ([], features, ['-DREPORT=32'], ['-DREPORT=32', *features]):
                with self.subTest(flags=flags):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-I', str(CORE), *flags, str(source), '-o', str(path / 'layers')], check=True)
                    subprocess.run([str(path / 'layers')], check=True)
