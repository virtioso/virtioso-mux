/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>

#include <camkes.h>
#include <camkes/io.h>
#include <platsupport/chardev.h>
#include <platsupport/plat/serial.h>

static ps_io_ops_t io_ops;
static struct ps_chardevice serial_device;
static struct ps_chardevice *serial = NULL;

typedef struct console_sink_batch_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} console_sink_batch_buffer_t;

static void sink_putchar(int c)
{
    if (serial != NULL) {
        ps_cdev_putchar(serial, c);
    }
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

    if (batch == NULL) {
        return;
    }

    while (batch->head != batch->tail) {
        sink_putchar((unsigned char)batch->buf[batch->head]);
        batch->head = (batch->head + 1) % sizeof(batch->buf);
    }
}

void getchar_foo(void)
{
}

int run(void)
{
    return 0;
}
