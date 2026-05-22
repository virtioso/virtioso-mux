/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <camkes.h>

typedef struct console_null_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_null_batch_buffer_t;

seL4_Word raw_batch_get_sender_id(void) WEAK;
void *raw_batch_buf(seL4_Word client_id) WEAK;

void raw_batch_batch(void)
{
    console_null_batch_buffer_t *batch =
        (console_null_batch_buffer_t *)raw_batch_buf(raw_batch_get_sender_id());

    if (batch == NULL) {
        return;
    }

    batch->head = batch->tail;
}
