/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <camkes.h>
#include <camkes/io.h>
#include <platsupport/chardev.h>
#include <platsupport/irq.h>
#include <platsupport/serial.h>
#include <platsupport/plat/serial.h>
#include <sel4/sel4.h>
#if defined(__i386__) || defined(__x86_64__)
#include <platsupport/arch/tsc.h>
#endif

#include "console_stream_ids.h"

#define CONSOLE_MUX_RPC_REPORT_INTERVAL 16
#define TCU_MUX_ESCAPE 0xfeU
#define TCU_MUX_DEFAULT 0x00U
#define TCU_MUX_CONTROL 0xfdU
#define TCU_MUX_CONTROL_STREAM_REGISTRY 0x02U

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
static int console_mux_registry_emitted;
static char console_mux_frame[4096 - 8];
static ps_io_ops_t console_mux_io_ops;
static struct ps_chardevice console_mux_serial_device;
static struct ps_chardevice *console_mux_serial;
static int console_mux_rx_stream = -1;
static int console_mux_rx_escape;

typedef struct console_mux_getchar_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_mux_getchar_buffer_t;

void *getchar_buf(seL4_Word client_id) WEAK;
void getchar_emit(unsigned int id) WEAK;
unsigned int getchar_num_badges(void) WEAK;

void getchar_foo(void)
{
}

static inline uint64_t console_mux_cycles_now(void)
{
#if defined(__i386__) || defined(__x86_64__)
    return rdtsc_pure();
#else
    return 0;
#endif
}

static const struct virtioso_camkes_mux_source *console_mux_source_for_badge(seL4_Word badge)
{
    for (size_t i = 0; i < virtioso_camkes_mux_source_count; i++) {
        if (virtioso_camkes_mux_sources[i].badge == badge) {
            return &virtioso_camkes_mux_sources[i];
        }
    }
    return NULL;
}

static int console_mux_valid_stream_id(int stream_id)
{
    return stream_id > 0 &&
        stream_id <= 0xff &&
        stream_id != TCU_MUX_ESCAPE &&
        stream_id != TCU_MUX_CONTROL;
}

static void console_mux_uplink_bytes(const char *bytes, uint32_t bytes_len)
{
    uint64_t start;

    start = console_mux_cycles_now();
    for (uint32_t i = 0; i < bytes_len; i++) {
        if (console_mux_serial != NULL) {
            ps_cdev_putchar(console_mux_serial, (unsigned char)bytes[i]);
        }
    }
    console_mux_rpc_stats.uplink_calls++;
    console_mux_rpc_stats.uplink_payload_bytes += bytes_len;
    console_mux_rpc_stats.uplink_cycles += console_mux_cycles_now() - start;
}

static void console_mux_emit_registry(void)
{
    size_t len;
    uint32_t out = 0;

    if (console_mux_registry_emitted) {
        return;
    }
    len = strlen(virtioso_camkes_stream_registry_json);
    if (len > 0xffff || len + 5 > sizeof(console_mux_frame)) {
        return;
    }

    console_mux_frame[out++] = TCU_MUX_ESCAPE;
    console_mux_frame[out++] = TCU_MUX_CONTROL;
    console_mux_frame[out++] = TCU_MUX_CONTROL_STREAM_REGISTRY;
    console_mux_frame[out++] = (char)((len >> 8) & 0xff);
    console_mux_frame[out++] = (char)(len & 0xff);
    memcpy(&console_mux_frame[out], virtioso_camkes_stream_registry_json, len);
    out += (uint32_t)len;

    console_mux_uplink_bytes(console_mux_frame, out);
    console_mux_registry_emitted = 1;
}

static void console_mux_emit_framed_payload(int stream_id, const char *bytes, uint32_t bytes_len)
{
    uint32_t out = 0;

    if (!console_mux_valid_stream_id(stream_id)) {
        return;
    }

    console_mux_emit_registry();

    console_mux_frame[out++] = TCU_MUX_ESCAPE;
    console_mux_frame[out++] = (char)stream_id;
    for (uint32_t i = 0; i < bytes_len; i++) {
        uint8_t byte = (uint8_t)bytes[i];
        if (out + 4 >= sizeof(console_mux_frame)) {
            console_mux_uplink_bytes(console_mux_frame, out);
            out = 0;
            console_mux_frame[out++] = TCU_MUX_ESCAPE;
            console_mux_frame[out++] = (char)stream_id;
        }
        if (byte == TCU_MUX_ESCAPE) {
            console_mux_frame[out++] = TCU_MUX_ESCAPE;
        }
        console_mux_frame[out++] = (char)byte;
    }
    if (out + 2 >= sizeof(console_mux_frame)) {
        console_mux_uplink_bytes(console_mux_frame, out);
        out = 0;
    }
    console_mux_frame[out++] = TCU_MUX_ESCAPE;
    console_mux_frame[out++] = TCU_MUX_DEFAULT;

    console_mux_uplink_bytes(console_mux_frame, out);
}

static void console_mux_emit_report_line(const char *line)
{
    (void)line;
}

static const struct virtioso_camkes_demux_sink *console_mux_sink_for_stream(int stream_id)
{
    for (size_t i = 0; i < virtioso_camkes_demux_sink_count; i++) {
        if (virtioso_camkes_demux_sinks[i].stream_id == stream_id) {
            return &virtioso_camkes_demux_sinks[i];
        }
    }
    return NULL;
}

static void console_mux_deliver_stream_byte(int stream_id, uint8_t byte)
{
    const struct virtioso_camkes_demux_sink *sink = console_mux_sink_for_stream(stream_id);
    volatile console_mux_getchar_buffer_t *rx;
    uint32_t next_tail;

    if (sink == NULL || getchar_buf == NULL || getchar_emit == NULL) {
        return;
    }

    rx = (volatile console_mux_getchar_buffer_t *)getchar_buf(sink->badge);
    if (rx == NULL) {
        return;
    }

    next_tail = (rx->tail + 1) % sizeof(rx->buf);
    if (next_tail == rx->head) {
        return;
    }

    rx->buf[rx->tail] = (char)byte;
    __sync_synchronize();
    rx->tail = next_tail;
    __sync_synchronize();
    getchar_emit(sink->badge);
}

static void console_mux_feed_downlink_byte(uint8_t byte)
{
    if (console_mux_rx_escape) {
        console_mux_rx_escape = 0;
        if (byte == TCU_MUX_DEFAULT) {
            console_mux_rx_stream = -1;
            return;
        }
        if (byte == TCU_MUX_ESCAPE) {
            if (console_mux_rx_stream >= 0) {
                console_mux_deliver_stream_byte(console_mux_rx_stream, byte);
            }
            return;
        }
        if (byte == TCU_MUX_CONTROL) {
            console_mux_rx_stream = -1;
            return;
        }
        if (console_mux_sink_for_stream(byte) != NULL) {
            console_mux_rx_stream = byte;
        } else {
            console_mux_rx_stream = -1;
        }
        return;
    }

    if (byte == TCU_MUX_ESCAPE) {
        console_mux_rx_escape = 1;
        return;
    }

    if (console_mux_rx_stream >= 0) {
        console_mux_deliver_stream_byte(console_mux_rx_stream, byte);
    }
}

static void console_mux_drain_uart(void)
{
    int ch;

    if (console_mux_serial == NULL) {
        return;
    }

    do {
        ch = ps_cdev_getchar(console_mux_serial);
        if (ch != EOF) {
            console_mux_feed_downlink_byte((uint8_t)ch);
        }
    } while (ch != EOF);
}

int serial_dev_irq_acknowledge(ps_irq_t *irq);

static void console_mux_handle_uart_irq(void)
{
    if (console_mux_serial != NULL) {
        console_mux_drain_uart();
        ps_cdev_handle_irq(console_mux_serial, 0);
        console_mux_drain_uart();
    }
}

void serial_dev_irq_handle(ps_irq_t *irq)
{
    console_mux_handle_uart_irq();
    int err = serial_dev_irq_acknowledge(irq);
    ZF_LOGE_IF(err != 0, "ConsoleMux failed to acknowledge UARTI IRQ");
}

void pre_init(void)
{
    int err = camkes_io_ops(&console_mux_io_ops);
    ZF_LOGF_IF(err != 0, "ConsoleMux failed to initialise IO ops");

    console_mux_serial = ps_cdev_init(
        PS_SERIAL_DEFAULT,
        &console_mux_io_ops,
        &console_mux_serial_device
    );
    ZF_LOGF_IF(console_mux_serial == NULL, "ConsoleMux failed to initialise UARTI");

    console_mux_serial->flags &= ~SERIAL_AUTO_CR;
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
    seL4_Word sender_id = mux_batch_get_sender_id();
    const struct virtioso_camkes_mux_source *source = console_mux_source_for_badge(sender_id);
    console_mux_batch_buffer_t *batch =
        (console_mux_batch_buffer_t *)mux_batch_buf(sender_id);
    uint32_t bytes_len;
    uint64_t start;

    if (source == NULL || batch == NULL) {
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
    console_mux_emit_framed_payload(source->stream_id, &batch->buf[batch->head], bytes_len);
    console_mux_rpc_stats.server_calls++;
    console_mux_rpc_stats.server_payload_bytes += bytes_len;
    console_mux_rpc_stats.server_cycles += console_mux_cycles_now() - start;
    if ((console_mux_rpc_stats.server_calls % CONSOLE_MUX_RPC_REPORT_INTERVAL) == 0) {
        console_mux_report_rpc_stats();
    }
    batch->head = batch->tail;
}
