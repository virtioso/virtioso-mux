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
#include <sel4/sel4.h>

static ps_io_ops_t io_ops;
static struct ps_chardevice serial_device;
static struct ps_chardevice *serial = NULL;
static int console_passthrough_logged_pre_init;
static int console_passthrough_logged_run;
static int console_passthrough_logged_first_raw;
static int console_passthrough_logged_first_processed;
static unsigned long console_passthrough_heartbeat_counter;

static void console_passthrough_debug_puts(const char *text)
{
    while (*text != '\0') {
        seL4_DebugPutChar(*text++);
    }
}

static void sink_putchar(int c)
{
    if (serial != NULL) {
        ps_cdev_putchar(serial, c);
    }
}

void pre_init(void)
{
    if (!console_passthrough_logged_pre_init) {
        console_passthrough_logged_pre_init = 1;
        console_passthrough_debug_puts("[cps pre_init]\n");
    }
    console_passthrough_debug_puts("[cps before io_ops]\n");
    int err = camkes_io_ops(&io_ops);
    assert(err == 0);
    console_passthrough_debug_puts("[cps after io_ops]\n");

    console_passthrough_debug_puts("[cps before cdev_init]\n");
    serial = ps_cdev_init(PS_SERIAL_DEFAULT, &io_ops, &serial_device);
    assert(serial != NULL);
    console_passthrough_debug_puts("[cps after cdev_init]\n");
}

void post_init(void)
{
    console_passthrough_debug_puts("[cps post_init]\n");
}

void raw_putchar__init(void)
{
    console_passthrough_debug_puts("[cps raw init]\n");
}

void getchar__init(void)
{
    console_passthrough_debug_puts("[cps getchar init]\n");
}

void serial_irq__init(void)
{
    console_passthrough_debug_puts("[cps irq init]\n");
}

void processed_putchar_putchar(int c)
{
    if (!console_passthrough_logged_first_processed) {
        console_passthrough_logged_first_processed = 1;
        console_passthrough_debug_puts("[cps first processed]\n");
    }
    if (c == '\n') {
        sink_putchar('\r');
    }
    sink_putchar(c);
}

void raw_putchar_putchar(int c)
{
    if (!console_passthrough_logged_first_raw) {
        console_passthrough_logged_first_raw = 1;
        console_passthrough_debug_puts("[cps first raw]\n");
    }
    sink_putchar(c);
}

void getchar_foo(void)
{
}

int run(void)
{
    if (!console_passthrough_logged_run) {
        console_passthrough_logged_run = 1;
        console_passthrough_debug_puts("[cps run]\n");
    }
    while (1) {
        console_passthrough_heartbeat_counter++;
        if (console_passthrough_heartbeat_counter == 50000000UL) {
            sink_putchar('P');
            console_passthrough_heartbeat_counter = 0;
        }
        seL4_Yield();
    }
    return 0;
}
