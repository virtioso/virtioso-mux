/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <camkes.h>
#include <sel4/sel4.h>

static volatile int console_mux_emit_lock;

typedef struct console_mux_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_mux_batch_buffer_t;

static void console_mux_uplink_bytes(const char *bytes, uint32_t bytes_len)
{
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)uplink_batch_get_buf();

    if (batch == NULL || bytes_len > sizeof(batch->buf)) {
        return;
    }

    batch->head = 0;
    batch->tail = 0;
    for (uint32_t i = 0; i < bytes_len; i++) {
        batch->buf[i] = bytes[i];
    }
    batch->tail = bytes_len;
    __sync_synchronize();
    uplink_batch_batch();
}

static void console_mux_lock(void)
{
    while (__atomic_test_and_set(&console_mux_emit_lock, __ATOMIC_ACQUIRE)) {
        seL4_Yield();
    }
}

static void console_mux_unlock(void)
{
    __atomic_clear(&console_mux_emit_lock, __ATOMIC_RELEASE);
}

void *mux_batch_buf(seL4_Word client_id) WEAK;
seL4_Word mux_batch_get_sender_id(void) WEAK;

void mux_batch_batch(void)
{
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)mux_batch_buf(mux_batch_get_sender_id());
    uint32_t bytes_len;

    if (batch == NULL) {
        return;
    }

    if (batch->tail < batch->head) {
        return;
    }

    bytes_len = batch->tail - batch->head;
    if (bytes_len == 0) {
        return;
    }

    console_mux_lock();
    console_mux_uplink_bytes(&batch->buf[batch->head], bytes_len);
    console_mux_unlock();
    batch->head = batch->tail;
}
