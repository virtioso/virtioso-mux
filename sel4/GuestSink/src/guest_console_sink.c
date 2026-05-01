/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <camkes.h>
#include <sel4/sel4.h>
#include <platsupport/arch/tsc.h>

#define TCU_MUX_ESCAPE 0xfeU
#define GUEST_CONSOLE_SINK_FLUSH_THRESHOLD 1024
#ifndef GUEST_CONSOLE_SINK_RPC_REPORTS
#define GUEST_CONSOLE_SINK_RPC_REPORTS 0
#endif
#ifdef GUEST_CONSOLE_SINK_DEBUGPUTCHAR_REPORTS
#define GUEST_CONSOLE_SINK_RPC_REPORT_INTERVAL 1
#else
#define GUEST_CONSOLE_SINK_RPC_REPORT_INTERVAL 16
#endif

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
static int guest_console_sink_debug_marked;

extern const char *get_instance_name(void);
extern int get_instance_console_stream_id(void);

static inline uint64_t guest_console_sink_cycles_now(void)
{
    return rdtsc_pure();
}

static guest_console_sink_batch_buffer_t *guest_console_sink_buffer(void)
{
    return (guest_console_sink_batch_buffer_t *)mux_batch_get_buf();
}

static int guest_console_sink_stream_id(void)
{
    return get_instance_console_stream_id();
}

static int guest_console_sink_is_interactive_prompt_stream(void)
{
    const char *name = get_instance_name();

    return name != NULL && strstr(name, "_guest_console_sink") != NULL;
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

static int guest_console_sink_append_or_flush(guest_console_sink_batch_buffer_t *batch, uint8_t byte)
{
    if (guest_console_sink_append_byte(batch, byte) == 0) {
        return 0;
    }

    guest_console_sink_flush(batch);
    return guest_console_sink_append_byte(batch, byte);
}

static int guest_console_sink_begin_stream(
    guest_console_sink_batch_buffer_t *batch,
    uint8_t stream_id
)
{
    if (batch->head != batch->tail) {
        return 0;
    }

    if (guest_console_sink_append_or_flush(batch, TCU_MUX_ESCAPE) != 0) {
        return -1;
    }
    return guest_console_sink_append_or_flush(batch, stream_id);
}

static void guest_console_sink_append_mux_byte(
    guest_console_sink_batch_buffer_t *batch,
    uint8_t stream_id,
    uint8_t byte
)
{
    if ((batch->tail + 4) >= sizeof(batch->buf)) {
        guest_console_sink_flush(batch);
    }

    if (guest_console_sink_begin_stream(batch, stream_id) != 0) {
        return;
    }

    if (byte == TCU_MUX_ESCAPE) {
        if (guest_console_sink_append_or_flush(batch, TCU_MUX_ESCAPE) != 0) {
            return;
        }
    }
    (void)guest_console_sink_append_or_flush(batch, byte);
}

static int guest_console_sink_valid_stream_id(int stream_id)
{
    if (stream_id < 0 || stream_id > 0xff) {
        return 0;
    }
    return stream_id != TCU_MUX_ESCAPE;
}

static void guest_console_sink_emit_report_line(guest_console_sink_batch_buffer_t *batch, const char *line)
{
#ifdef GUEST_CONSOLE_SINK_DEBUGPUTCHAR_REPORTS
    (void)batch;
    while (*line != '\0') {
        seL4_DebugPutChar(*line++);
    }
#else
    int local_stream_id = guest_console_sink_stream_id();

    if (!guest_console_sink_valid_stream_id(local_stream_id)) {
        return;
    }
    while (*line != '\0') {
        guest_console_sink_append_mux_byte(batch, (uint8_t)local_stream_id, (uint8_t)*line++);
    }
    guest_console_sink_flush(batch);
#endif
}

static void guest_console_sink_report_rpc_stats(guest_console_sink_batch_buffer_t *batch)
{
    char line[192];
    int len = snprintf(
        line,
        sizeof(line),
        "\n[rpcprof] gcs caller stream=%d calls=%llu payload=%llu cyc=%llu avg_call=%llu avg_byte=%llu\n",
        guest_console_sink_stream_id(),
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
#if GUEST_CONSOLE_SINK_RPC_REPORTS
    if ((guest_console_sink_rpc_stats.batch_calls % GUEST_CONSOLE_SINK_RPC_REPORT_INTERVAL) == 0) {
        guest_console_sink_report_rpc_stats(batch);
    }
#endif
    guest_console_sink_reset(batch);
}

static void guest_console_sink_emit_mux_byte(uint8_t stream_id, uint8_t byte)
{
    guest_console_sink_batch_buffer_t *batch = guest_console_sink_buffer();

    if (batch == NULL) {
        return;
    }

    if (batch->tail < batch->head) {
        guest_console_sink_reset(batch);
    }

    guest_console_sink_append_mux_byte(batch, stream_id, byte);

    if (byte == '\n' || byte == '\r' ||
        (byte == ':' && guest_console_sink_is_interactive_prompt_stream()) ||
        batch->tail >= GUEST_CONSOLE_SINK_FLUSH_THRESHOLD) {
        guest_console_sink_flush(batch);
    }
}

void guest_putchar_putchar(int c)
{
    int local_stream_id = guest_console_sink_stream_id();

    if (!guest_console_sink_valid_stream_id(local_stream_id)) {
        return;
    }
#ifdef GUEST_CONSOLE_SINK_DEBUGPUTCHAR_REPORTS
    if (!guest_console_sink_debug_marked) {
        const char *marker = "\n[gcs first-call]\n";
        guest_console_sink_debug_marked = 1;
        while (*marker != '\0') {
            seL4_DebugPutChar(*marker++);
        }
    }
#endif
    guest_console_sink_emit_mux_byte((uint8_t)local_stream_id, (uint8_t)c);
}
