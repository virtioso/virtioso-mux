/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>

#include <camkes.h>
#include <platsupport/arch/tsc.h>
#define CONSOLE_MUX_RPC_REPORT_INTERVAL 16
#define TCU_MUX_ESCAPE 0xfeU

typedef struct console_mux_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_mux_batch_buffer_t;

typedef struct console_mux_rpc_stats {
    uint64_t server_calls;
    uint64_t server_payload_bytes;
    uint64_t server_cycles;
    uint64_t uplink_calls;
    uint64_t uplink_payload_bytes;
    uint64_t uplink_cycles;
} console_mux_rpc_stats_t;

static console_mux_rpc_stats_t console_mux_rpc_stats;

extern int get_instance_console_stream_id(void);

static inline uint64_t console_mux_cycles_now(void)
{
    return rdtsc_pure();
}

static int console_mux_stream_id(void)
{
    return get_instance_console_stream_id();
}

static void console_mux_uplink_bytes(const char *bytes, uint32_t bytes_len)
{
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)uplink_batch_get_buf();
    uint64_t start;

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
    start = console_mux_cycles_now();
    uplink_batch_batch();
    console_mux_rpc_stats.uplink_calls++;
    console_mux_rpc_stats.uplink_payload_bytes += bytes_len;
    console_mux_rpc_stats.uplink_cycles += console_mux_cycles_now() - start;
}

static void console_mux_emit_report_line(const char *line)
{
    char framed[2 + (2 * 256)];
    int local_stream_id = console_mux_stream_id();
    uint32_t out = 0;

    if (local_stream_id < 0 || local_stream_id > 0xff || local_stream_id == TCU_MUX_ESCAPE) {
        return;
    }

    framed[out++] = TCU_MUX_ESCAPE;
    framed[out++] = (uint8_t)local_stream_id;
    while (*line != '\0' && (out + 2) <= sizeof(framed)) {
        uint8_t byte = (uint8_t)*line++;
        if (byte == TCU_MUX_ESCAPE) {
            framed[out++] = TCU_MUX_ESCAPE;
        }
        framed[out++] = (char)byte;
    }

    if (out != 0) {
        console_mux_uplink_bytes(framed, out);
    }
}

static void console_mux_report_rpc_stats(void)
{
    char line[256];
    int len = snprintf(
        line,
        sizeof(line),
        "\n[rpcprof] mux srv_calls=%llu srv_bytes=%llu srv_cyc=%llu srv_avg=%llu uplink_calls=%llu uplink_bytes=%llu uplink_cyc=%llu uplink_avg=%llu\n",
        (unsigned long long)console_mux_rpc_stats.server_calls,
        (unsigned long long)console_mux_rpc_stats.server_payload_bytes,
        (unsigned long long)console_mux_rpc_stats.server_cycles,
        (unsigned long long)(
            console_mux_rpc_stats.server_calls ?
            console_mux_rpc_stats.server_cycles / console_mux_rpc_stats.server_calls : 0
        ),
        (unsigned long long)console_mux_rpc_stats.uplink_calls,
        (unsigned long long)console_mux_rpc_stats.uplink_payload_bytes,
        (unsigned long long)console_mux_rpc_stats.uplink_cycles,
        (unsigned long long)(
            console_mux_rpc_stats.uplink_calls ?
            console_mux_rpc_stats.uplink_cycles / console_mux_rpc_stats.uplink_calls : 0
        )
    );

    if (len > 0) {
        console_mux_emit_report_line(line);
    }
}

void *mux_batch_buf(seL4_Word client_id) WEAK;
seL4_Word mux_batch_get_sender_id(void) WEAK;

void mux_batch_batch(void)
{
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)mux_batch_buf(mux_batch_get_sender_id());
    uint32_t bytes_len;
    uint64_t start;

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

    start = console_mux_cycles_now();
    console_mux_uplink_bytes(&batch->buf[batch->head], bytes_len);
    console_mux_rpc_stats.server_calls++;
    console_mux_rpc_stats.server_payload_bytes += bytes_len;
    console_mux_rpc_stats.server_cycles += console_mux_cycles_now() - start;
    if ((console_mux_rpc_stats.server_calls % CONSOLE_MUX_RPC_REPORT_INTERVAL) == 0) {
        console_mux_report_rpc_stats();
    }
    batch->head = batch->tail;
}
