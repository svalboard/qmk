// The transaction an EXECUTE_RPC on the other half may run
// (quantum/split_common/split_rpc_id.h, used by transactions.c's
// slave_rpc_exec_callback): rpc_info comes from the wire behind a CRC-8 only,
// so every byte its int8_t transaction_id can hold is tried. Only the RPC IDs
// (after GET_RPC_RESP_DATA, below NUM_TOTAL_TRANSACTIONS) may index
// split_transaction_table; a byte of 0x80 or more, a core ID and EXECUTE_RPC
// itself never run. Built with Svalboard's IDs (keyboards/svalboard/config.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "split_rpc_id.h"

int main(void) {
    int runs = 0;
    for (int b = 0; b < 256; b++) {
        uint8_t byte = (uint8_t)b;
        int8_t  id;
        memcpy(&id, &byte, 1); // as rpc_info holds the byte from the wire
        bool want = b > GET_RPC_RESP_DATA && b < NUM_TOTAL_TRANSACTIONS;
        if (split_rpc_exec_id_ok(id) != want) {
            printf("FAIL: transaction id byte 0x%02X %s\n", b, want ? "refused" : "would run");
            return 1;
        }
        runs += want;
    }
    assert(split_rpc_exec_id_ok(KEYBOARD_SYNC_A) && split_rpc_exec_id_ok(KEYBOARD_SYNC_B) && split_rpc_exec_id_ok(KEYBOARD_UPDATE));
    assert(!split_rpc_exec_id_ok(EXECUTE_RPC) && !split_rpc_exec_id_ok(PUT_RPC_INFO) && !split_rpc_exec_id_ok(GET_SLAVE_MATRIX_CHECKSUM));
    assert(!split_rpc_exec_id_ok((int8_t)-128) && !split_rpc_exec_id_ok((int8_t)-1));
    printf("split RPC ids: %d of 256 transaction id bytes run (the RPCs); 0x80-0xFF, the core ids and EXECUTE_RPC never index the table\n", runs);
    return 0;
}
