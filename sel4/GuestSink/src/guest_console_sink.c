/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <camkes.h>
#include <platsupport/arch/tsc.h>

#define CONSOLE_FRAME_MAGIC_0 'C'
#define CONSOLE_FRAME_MAGIC_1 'F'
#define CONSOLE_FRAME_VERSION 1
#define CONSOLE_FRAME_DIRECTION_RX 1
#define CONSOLE_FRAME_FLAGS 0
#define CONSOLE_FRAME_STREAM_VMM_DEBUG 7
#define GUEST_CONSOLE_SINK_FLUSH_THRESHOLD 1024
#define GUEST_CONSOLE_SINK_RPC_REPORT_INTERVAL 16

typedef struct guest_console_sink_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} guest_console_sink_batch_buffer_t;

typedef struct guest_console_sink_rpc_stats {
    uint64_t batch_calls;
    uint64_t batch_payload_bytes;
    uint64_t batch_cycles;
} guest_console_sink_rpc_stats_t;

static guest_console_sink_rpc_stats_t guest_console_sink_rpc_stats;

static inline uint64_t guest_console_sink_cycles_now(void)
{
    return rdtsc_pure();
}

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

static void guest_console_sink_flush(guest_console_sink_batch_buffer_t *batch);

static void guest_console_sink_append_frame(
    guest_console_sink_batch_buffer_t *batch,
    uint8_t framed_stream_id,
    uint8_t byte
)
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

    for (size_t i = 0; i < sizeof(frame); i++) {
        if (guest_console_sink_append_byte(batch, frame[i]) != 0) {
            guest_console_sink_flush(batch);
            if (guest_console_sink_append_byte(batch, frame[i]) != 0) {
                return;
            }
        }
    }
}

static void guest_console_sink_emit_report_line(guest_console_sink_batch_buffer_t *batch, const char *line)
{
    while (*line != '\0') {
        guest_console_sink_append_frame(batch, CONSOLE_FRAME_STREAM_VMM_DEBUG, (uint8_t)*line++);
    }
    guest_console_sink_flush(batch);
}

static void guest_console_sink_report_rpc_stats(guest_console_sink_batch_buffer_t *batch)
{
    char line[192];
    int len = snprintf(
        line,
        sizeof(line),
        "\n[rpcprof] gcs caller stream=%d calls=%llu payload=%llu cyc=%llu avg_call=%llu avg_byte=%llu\n",
        stream_id,
        (unsigned long long)guest_console_sink_rpc_stats.batch_calls,
        (unsigned long long)guest_console_sink_rpc_stats.batch_payload_bytes,
        (unsigned long long)guest_console_sink_rpc_stats.batch_cycles,
        (unsigned long long)(
            guest_console_sink_rpc_stats.batch_calls ?
            guest_console_sink_rpc_stats.batch_cycles / guest_console_sink_rpc_stats.batch_calls : 0
        ),
        (unsigned long long)(
            guest_console_sink_rpc_stats.batch_payload_bytes ?
            guest_console_sink_rpc_stats.batch_cycles / guest_console_sink_rpc_stats.batch_payload_bytes : 0
        )
    );

    if (len > 0) {
        guest_console_sink_emit_report_line(batch, line);
    }
}

static void guest_console_sink_flush(guest_console_sink_batch_buffer_t *batch)
{
    uint32_t payload_bytes;
    uint64_t start;

    if (batch == NULL || batch->head == batch->tail) {
        return;
    }

    payload_bytes = batch->tail - batch->head;
    __sync_synchronize();
    start = guest_console_sink_cycles_now();
    mux_batch_batch();
    guest_console_sink_rpc_stats.batch_calls++;
    guest_console_sink_rpc_stats.batch_payload_bytes += payload_bytes;
    guest_console_sink_rpc_stats.batch_cycles += guest_console_sink_cycles_now() - start;
    if ((guest_console_sink_rpc_stats.batch_calls % GUEST_CONSOLE_SINK_RPC_REPORT_INTERVAL) == 0) {
        guest_console_sink_report_rpc_stats(batch);
    }
    guest_console_sink_reset(batch);
}

static void guest_console_sink_emit_frame_byte(uint8_t framed_stream_id, uint8_t byte)
{
    guest_console_sink_batch_buffer_t *batch = guest_console_sink_buffer();

    if (batch == NULL) {
        return;
    }

    if (batch->tail < batch->head) {
        guest_console_sink_reset(batch);
    }

    guest_console_sink_append_frame(batch, framed_stream_id, byte);

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
