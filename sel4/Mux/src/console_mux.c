/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

#include <camkes.h>
#include <sel4/sel4.h>

#define CONSOLE_FRAME_MAGIC_0 'C'
#define CONSOLE_FRAME_MAGIC_1 'F'
#define CONSOLE_FRAME_VERSION 1
#define CONSOLE_FRAME_DIRECTION_RX 1
#define CONSOLE_FRAME_FLAGS 0

static volatile int console_mux_emit_lock;

typedef struct console_mux_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_mux_batch_buffer_t;

static void console_mux_uplink_frame(const uint8_t *frame, size_t frame_len)
{
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)uplink_batch_get_buf();

    if (batch == NULL || frame_len > sizeof(batch->buf)) {
        return;
    }

    batch->head = 0;
    batch->tail = 0;
    for (size_t i = 0; i < frame_len; i++) {
        batch->buf[i] = (char)frame[i];
    }
    batch->tail = (uint32_t)frame_len;
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

static void console_mux_emit_frame_byte(uint8_t stream_id, uint8_t byte)
{
    uint8_t frame[] = {
        CONSOLE_FRAME_MAGIC_0,
        CONSOLE_FRAME_MAGIC_1,
        CONSOLE_FRAME_VERSION,
        stream_id,
        CONSOLE_FRAME_DIRECTION_RX,
        CONSOLE_FRAME_FLAGS,
        0,
        0,
        0,
        1,
        byte,
    };

    console_mux_lock();
    console_mux_uplink_frame(frame, sizeof(frame));
    console_mux_unlock();
}

void mux_emit_emit(int stream_id, int c)
{
    if (stream_id < 0 || stream_id > 0xff) {
        return;
    }
    console_mux_emit_frame_byte((uint8_t)stream_id, (uint8_t)c);
}
