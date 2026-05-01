/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <camkes.h>
#include <camkes/io.h>
#include <platsupport/chardev.h>
#include <platsupport/plat/serial.h>
#include <platsupport/arch/tsc.h>
#include <sel4/sel4.h>

#define CONSOLE_SINK_RPC_REPORT_INTERVAL 16
#ifndef CONSOLE_SINK_RPC_REPORTS
#define CONSOLE_SINK_RPC_REPORTS 1
#endif

static ps_io_ops_t io_ops;
static struct ps_chardevice serial_device;
static struct ps_chardevice *serial = NULL;

typedef struct console_sink_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_sink_batch_buffer_t;

typedef struct console_sink_rpc_stats {
    uint64_t server_calls;
    uint64_t server_payload_bytes;
    uint64_t server_cycles;
} console_sink_rpc_stats_t;

static console_sink_rpc_stats_t console_sink_rpc_stats;

static inline uint64_t console_sink_cycles_now(void)
{
    return rdtsc_pure();
}

static void console_sink_debug_puts(const char *str)
{
    while (*str != '\0') {
        seL4_DebugPutChar(*str++);
    }
}

static void console_sink_report_rpc_stats(void)
{
    char line[192];
    int len = snprintf(
        line,
        sizeof(line),
        "\n[rpcprof] sink srv_calls=%llu srv_bytes=%llu srv_cyc=%llu srv_avg=%llu\n",
        (unsigned long long)console_sink_rpc_stats.server_calls,
        (unsigned long long)console_sink_rpc_stats.server_payload_bytes,
        (unsigned long long)console_sink_rpc_stats.server_cycles,
        (unsigned long long)(
            console_sink_rpc_stats.server_calls ?
            console_sink_rpc_stats.server_cycles / console_sink_rpc_stats.server_calls : 0
        )
    );

    if (len > 0) {
        console_sink_debug_puts(line);
    }
}

static void sink_putchar(int c)
{
#ifdef CONSOLE_SINK_DROP_OUTPUT
    (void)c;
    return;
#else
    if (serial != NULL) {
        ps_cdev_putchar(serial, c);
    }
#endif
}

void pre_init(void)
{
    int err = camkes_io_ops(&io_ops);
    assert(err == 0);

    serial = ps_cdev_init(PS_SERIAL_DEFAULT, &io_ops, &serial_device);
    assert(serial != NULL);
}

void processed_putchar_putchar(int c)
{
    if (c == '\n') {
        sink_putchar('\r');
    }
    sink_putchar(c);
}

void raw_putchar_putchar(int c)
{
    sink_putchar(c);
}

seL4_Word raw_batch_get_sender_id(void) WEAK;
void *raw_batch_buf(seL4_Word client_id) WEAK;

void raw_batch_batch(void)
{
    console_sink_batch_buffer_t *batch =
        (console_sink_batch_buffer_t *)raw_batch_buf(raw_batch_get_sender_id());
    uint64_t start;
    uint64_t payload_bytes = 0;

    if (batch == NULL) {
        return;
    }

    start = console_sink_cycles_now();
    while (batch->head != batch->tail) {
        sink_putchar((unsigned char)batch->buf[batch->head]);
        batch->head = (batch->head + 1) % sizeof(batch->buf);
        payload_bytes++;
    }

    if (payload_bytes != 0) {
        console_sink_rpc_stats.server_calls++;
        console_sink_rpc_stats.server_payload_bytes += payload_bytes;
        console_sink_rpc_stats.server_cycles += console_sink_cycles_now() - start;
#if CONSOLE_SINK_RPC_REPORTS
        if ((console_sink_rpc_stats.server_calls % CONSOLE_SINK_RPC_REPORT_INTERVAL) == 0) {
            console_sink_report_rpc_stats();
        }
#endif
    }
}

void getchar_foo(void)
{
}

int run(void)
{
    return 0;
}
