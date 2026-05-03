/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>

#include <camkes.h>
#include <sel4/sel4.h>
#if defined(__i386__) || defined(__x86_64__)
#include <platsupport/arch/tsc.h>
#endif

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
    uint32_t stream_id;
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 12];
} guest_console_sink_batch_buffer_t;

typedef struct guest_console_sink_rpc_stats {
    uint64_t batch_calls;
    uint64_t batch_payload_bytes;
    uint64_t batch_cycles;
} guest_console_sink_rpc_stats_t;

static guest_console_sink_rpc_stats_t guest_console_sink_rpc_stats;
static int guest_console_sink_debug_marked;

static inline uint64_t guest_console_sink_cycles_now(void)
{
#if defined(__i386__) || defined(__x86_64__)
    return rdtsc_pure();
#else
    return 0;
#endif
}

static guest_console_sink_batch_buffer_t *guest_console_sink_buffer(void)
{
    return (guest_console_sink_batch_buffer_t *)mux_batch_get_buf();
}

static void guest_console_sink_reset(guest_console_sink_batch_buffer_t *batch)
{
    batch->stream_id = (uint32_t)get_instance_console_stream_id();
    batch->head = 0;
    batch->tail = 0;
}

static int guest_console_sink_append_byte(guest_console_sink_batch_buffer_t *batch, uint8_t byte)
{
    if (batch->tail >= sizeof(batch->buf)) {
        return -1;
    }

    batch->buf[batch->tail] = (char)byte;
    batch->tail++;
    return 0;
}

static void guest_console_sink_flush(guest_console_sink_batch_buffer_t *batch);

static void guest_console_sink_append_or_flush(guest_console_sink_batch_buffer_t *batch, uint8_t byte)
{
    if (guest_console_sink_append_byte(batch, byte) == 0) {
        return;
    }

    guest_console_sink_flush(batch);
    (void)guest_console_sink_append_byte(batch, byte);
}

static void guest_console_sink_emit_report_line(guest_console_sink_batch_buffer_t *batch, const char *line)
{
#ifdef GUEST_CONSOLE_SINK_DEBUGPUTCHAR_REPORTS
    (void)batch;
    while (*line != '\0') {
        seL4_DebugPutChar(*line++);
    }
#else
    while (*line != '\0') {
        guest_console_sink_append_or_flush(batch, (uint8_t)*line++);
    }
    guest_console_sink_flush(batch);
#endif
}

static void guest_console_sink_report_rpc_stats(guest_console_sink_batch_buffer_t *batch)
{
    char line[160];
    int len = snprintf(
        line,
        sizeof(line),
        "\n[rpcprof] gcs caller calls=%llu payload=%llu cyc=%llu avg_call=%llu avg_byte=%llu\n",
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

    if (batch->tail < batch->head || batch->tail > sizeof(batch->buf)) {
        guest_console_sink_reset(batch);
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

static void guest_console_sink_emit_byte(uint8_t byte)
{
    guest_console_sink_batch_buffer_t *batch = guest_console_sink_buffer();

    if (batch == NULL) {
        return;
    }

    if (batch->tail < batch->head || batch->tail > sizeof(batch->buf)) {
        guest_console_sink_reset(batch);
    }
    if ((int)batch->stream_id != get_instance_console_stream_id()) {
        guest_console_sink_reset(batch);
    }

    guest_console_sink_append_or_flush(batch, byte);

    if (byte == '\n' || byte == '\r' ||
        byte == ':' ||
        batch->tail >= GUEST_CONSOLE_SINK_FLUSH_THRESHOLD) {
        guest_console_sink_flush(batch);
    }
}

void guest_putchar_putchar(int c)
{
#ifdef GUEST_CONSOLE_SINK_DEBUGPUTCHAR_REPORTS
    if (!guest_console_sink_debug_marked) {
        const char *marker = "\n[gcs first-call]\n";
        guest_console_sink_debug_marked = 1;
        while (*marker != '\0') {
            seL4_DebugPutChar(*marker++);
        }
    }
#endif
    guest_console_sink_emit_byte((uint8_t)c);
}
