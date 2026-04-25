/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>

void guest_putchar_putchar(int c)
{
    if (stream_id < 0 || stream_id > 0xff) {
        return;
    }
    mux_emit_emit(stream_id, c);
}
