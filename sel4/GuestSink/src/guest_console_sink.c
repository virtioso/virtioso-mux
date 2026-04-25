/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

#include <camkes.h>

#define CONSOLE_FRAME_MAGIC_0 'C'
#define CONSOLE_FRAME_MAGIC_1 'F'
#define CONSOLE_FRAME_VERSION 1
#define CONSOLE_FRAME_DIRECTION_RX 1
#define CONSOLE_FRAME_FLAGS 0
#define GUEST_CONSOLE_SINK_FLUSH_THRESHOLD 1024

typedef struct guest_console_sink_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} guest_console_sink_batch_buffer_t;

static guest_console_sink_batch_buffer_t *guest_console_sink_buffer(void)
{
    return (guest_console_sink_batch_buffer_t *)mux_batch_get_buf();
}

static int guest_console_sink_is_interactive_prompt_stream(uint8_t framed_stream_id)
{
    return framed_stream_id == 1 || framed_stream_id == 5;
}

static void guest_console_sink_reset(guest_console_sink_batch_buffer_t *batch)
{
    batch->head = 0;
    batch->tail = 0;
}

static void guest_console_sink_flush(guest_console_sink_batch_buffer_t *batch)
{
    if (batch == NULL || batch->head == batch->tail) {
        return;
    }

    __sync_synchronize();
    mux_batch_batch();
    guest_console_sink_reset(batch);
}

static int guest_console_sink_append_byte(guest_console_sink_batch_buffer_t *batch, uint8_t byte)
{
    uint32_t next_tail = (batch->tail + 1) % sizeof(batch->buf);

    if (next_tail == batch->head) {
        return -1;
    }

    batch->buf[batch->tail] = (char)byte;
    batch->tail = next_tail;
    return 0;
}

static void guest_console_sink_emit_frame_byte(uint8_t framed_stream_id, uint8_t byte)
{
    uint8_t frame[] = {
        CONSOLE_FRAME_MAGIC_0,
        CONSOLE_FRAME_MAGIC_1,
        CONSOLE_FRAME_VERSION,
        framed_stream_id,
        CONSOLE_FRAME_DIRECTION_RX,
        CONSOLE_FRAME_FLAGS,
        0,
        0,
        0,
        1,
        byte,
    };
    guest_console_sink_batch_buffer_t *batch = guest_console_sink_buffer();

    if (batch == NULL) {
        return;
    }

    if (batch->tail < batch->head) {
        guest_console_sink_reset(batch);
    }

    for (size_t i = 0; i < sizeof(frame); i++) {
        if (guest_console_sink_append_byte(batch, frame[i]) != 0) {
            guest_console_sink_flush(batch);
            if (guest_console_sink_append_byte(batch, frame[i]) != 0) {
                return;
            }
        }
    }

    if (byte == '\n' || byte == '\r' ||
        (byte == ':' && guest_console_sink_is_interactive_prompt_stream(framed_stream_id)) ||
        batch->tail >= GUEST_CONSOLE_SINK_FLUSH_THRESHOLD) {
        guest_console_sink_flush(batch);
    }
}

void guest_putchar_putchar(int c)
{
    if (stream_id < 0 || stream_id > 0xff) {
        return;
    }
    guest_console_sink_emit_frame_byte((uint8_t)stream_id, (uint8_t)c);
}
