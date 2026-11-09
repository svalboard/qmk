"""Production layer resolver, source cache and protocol exercised without a board."""
import pathlib
import re
import unittest

from test_regressions import PRELUDE, run_c

ROOT = pathlib.Path(__file__).resolve().parents[2]


def layer_function(name):
    source = (ROOT / 'quantum/action_layer.c').read_text()
    start = re.search(r'^\w[^\n]*\b' + name + r'\([^\n]*\) \{', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


MOCKS = r'''
#include <limits.h>
#define LAYER_SWITCH_GET_LAYER_OVERRIDE
#define DYNAMIC_KEYMAP_LAYER_COUNT 16
#define MAX_LAYER 32
#define MAX_LAYER_BITS 5
#define MATRIX_ROWS 1
#define MATRIX_COLS 2
#define ACTION_TRANSPARENT 1
enum { sval_cmd_context_layer_set=0x26,sval_cmd_context_layer_status,sval_cmd_context_layer_renew,sval_cmd_context_layer_clear };
typedef uint32_t layer_state_t;
typedef struct { uint8_t row,col; } keypos_t;
typedef struct { uint16_t code; } action_t;
static layer_state_t layer_state,default_layer_state=1;
static uint32_t now;
static bool disable_action_cache;
static uint16_t keys[32][2];
static uint8_t source_layers_cache[1][MAX_LAYER_BITS];
uint32_t timer_read32(void) { return now; }
uint32_t timer_elapsed32(uint32_t since) { return now-since; }
void layer_state_set(layer_state_t state) { layer_state=state; }
action_t action_for_key(uint8_t layer,keypos_t key) { return (action_t){keys[layer][key.col]}; }
action_t layer_switch_get_action(keypos_t key);
'''

HELPERS = r'''
action_t layer_switch_get_action(keypos_t key) { return action_for_key(layer_switch_get_layer(key),key); }
static uint8_t packet[32];
static unsigned command(uint8_t cmd,uint16_t tx,uint8_t layer) {
 memset(packet,0,sizeof(packet)); packet[1]=cmd; packet[2]=tx; packet[3]=tx>>8; packet[4]=layer;
 assert(sval_context_layer_command(packet,32)); return packet[2];
}
static void init_keys(void) { for(unsigned i=0;i<32;i++) for(unsigned k=0;k<2;k++) keys[i][k]=ACTION_TRANSPARENT; keys[0][0]=4; keys[0][1]=5; }
'''


class ContextLayerRegressions(unittest.TestCase):
    def test_layer_ownership_priority_and_cached_release(self):
        context = '\n'.join(line for line in (ROOT / 'modules/svalboard/core/sval_context_layer.c').read_text().splitlines() if not line.startswith('#include'))
        functions = ''.join(
            layer_function(name)
            for name in ['layer_switch_get_layer', 'update_source_layers_cache_impl', 'read_source_layers_cache_impl', 'update_source_layers_cache', 'read_source_layers_cache', 'store_or_get_action', 'layer_on', 'layer_off', 'layer_move', 'layer_clear']
        )
        run_c(
            PRELUDE + MOCKS + context + functions + HELPERS + r'''
int main(void) {
 keypos_t k={0,0}; init_keys(); keys[9][0]=9; keys[2][0]=2; keys[10][0]=10;
 assert(layer_switch_get_layer(k)==0);
 assert(!command(0x26,1,9)); assert(layer_switch_get_layer(k)==9 && layer_state==0);
 layer_on(2); assert(layer_switch_get_layer(k)==2); // lower manual index wins
 layer_off(2); assert(layer_switch_get_layer(k)==9); // manual release preserves app
 layer_on(9); assert(!command(0x29,0,0)); assert(layer_state==(1u<<9));
 assert(layer_switch_get_layer(k)==9); // clearing app never clears same manual bit
 layer_clear(); assert(!command(0x26,2,9));
 assert(store_or_get_action(true,k).code==9);
 assert(!command(0x26,3,10));
 assert(store_or_get_action(false,k).code==9); // release uses press source
 assert(store_or_get_action(true,k).code==10);
 assert(!command(0x29,0,0)); assert(store_or_get_action(false,k).code==10);
 assert(layer_switch_get_layer(k)==0);
 assert(!command(0x26,4,9)); layer_move(0); assert(layer_switch_get_layer(k)==9);
 // TO(base) preserves automatic context; companion pause is the bypass.
 keys[9][0]=ACTION_TRANSPARENT; assert(layer_switch_get_layer(k)==0);
 default_layer_state=1u<<3; keys[3][0]=33;
 assert(layer_switch_get_layer(k)==3); // transparent app falls to selected default
 keys[9][0]=9; layer_move(3); assert(layer_switch_get_layer(k)==9);
 assert(command(0x26,0,9)==1); assert(command(0x26,5,16)==1);
 assert(command(0x28,3,0)==1); now=4999; assert(!command(0x28,4,0));
 now=9998; assert(sval_context_layer()==9); now=9999; assert(sval_context_layer()==255);
 assert(layer_state==(1u<<3) && default_layer_state==(1u<<3));
 assert(command(0x28,4,0)==1); // an expired context cannot be renewed
 now=UINT32_MAX-100; assert(!command(0x26,6,9)); now=4899;
 assert(sval_context_layer()==255); // lease uses wrap-safe timer
 assert(!command(0x27,0,0)); assert(packet[3]==1 && packet[4]==16 && packet[5]==255);
 assert(packet[10]==8 && packet[14]==8 && packet[18]==8);
 for(unsigned n=0;n<5;n++) {packet[1]=0x26;packet[2]=1;packet[3]=0;assert(!sval_context_layer_command(packet,n));}
 return 0;
}
'''
        )
