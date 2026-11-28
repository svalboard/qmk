#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "matrix.h"

void split_pre_init(void);
void split_post_init(void);

bool transport_master_if_connected(matrix_row_t master_matrix[], matrix_row_t slave_matrix[]);
bool is_transport_connected(void);
// Svalboard: the exchange starts again after a stop on purpose (the split
// pause): no run of failed scans carries over the stop (split_connection.h).
void transport_connection_restart(void);

void split_watchdog_update(bool done);
void split_watchdog_task(void);
bool split_watchdog_check(void);
