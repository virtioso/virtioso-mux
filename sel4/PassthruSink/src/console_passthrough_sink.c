/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>

#include <camkes.h>
#include <camkes/io.h>
#include <platsupport/chardev.h>
#include <platsupport/plat/serial.h>

static ps_io_ops_t io_ops;
static struct ps_chardevice serial_device;
static struct ps_chardevice *serial = NULL;

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

void getchar_foo(void)
{
}

int run(void)
{
    while (1) {
        seL4_Yield();
    }
    return 0;
}
