/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <camkes.h>
#include <sel4/sel4.h>

#define CONSOLE_FRAME_MAGIC_0 'C'
#define CONSOLE_FRAME_MAGIC_1 'F'
#define CONSOLE_FRAME_VERSION 1
#define CONSOLE_FRAME_DIRECTION_RX 1
#define CONSOLE_FRAME_FLAGS 0

static volatile int console_mux_emit_lock;

static void console_mux_uplink_byte(uint8_t byte)
{
    uplink_putchar(byte);
}

static void console_mux_lock(void)
{
    while (__atomic_test_and_set(&console_mux_emit_lock, __ATOMIC_ACQUIRE)) {
        seL4_Yield();
    }
}

static void console_mux_unlock(void)
{
    __atomic_clear(&console_mux_emit_lock, __ATOMIC_RELEASE);
}

static void console_mux_emit_frame_byte(uint8_t stream_id, uint8_t byte)
{
    console_mux_lock();
    console_mux_uplink_byte(CONSOLE_FRAME_MAGIC_0);
    console_mux_uplink_byte(CONSOLE_FRAME_MAGIC_1);
    console_mux_uplink_byte(CONSOLE_FRAME_VERSION);
    console_mux_uplink_byte(stream_id);
    console_mux_uplink_byte(CONSOLE_FRAME_DIRECTION_RX);
    console_mux_uplink_byte(CONSOLE_FRAME_FLAGS);
    console_mux_uplink_byte(0);
    console_mux_uplink_byte(0);
    console_mux_uplink_byte(0);
    console_mux_uplink_byte(1);
    console_mux_uplink_byte(byte);
    console_mux_unlock();
}

void mux_emit_emit(int stream_id, int c)
{
    if (stream_id < 0 || stream_id > 0xff) {
        return;
    }
    console_mux_emit_frame_byte((uint8_t)stream_id, (uint8_t)c);
}
