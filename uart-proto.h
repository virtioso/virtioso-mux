/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright (c) 2013-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_UART_PROTO_H
#define INCLUDE_UART_PROTO_H

/**
 * @defgroup virt_uart_protocol_codes Virtualized UART Protocol Codes
 * @{
 */
/// Specifies the character to use as a beginning of an ESC sequence.
/// The character afterwards is a "command"
#define UART_PROTO_ESC_START 0xffU

/// Command send actual character that is being used for ESC start
/// (Think %% in printfs)
#define UART_PROTO_ESC_ESC   0xffU

/// CH < 16 are used to switch destination to guest number CH

/// Switch to "hypervisor output"
#define UART_PROTO_ESC_ID_HYP 0xfeU

/// Specifies to restart.
/// Party sending this command just restarted and will assume start
/// conditions
#define UART_PROTO_ESC_RESET   0xfdU

/// Specifies to query the list of guest names.
#define UART_PROTO_ESC_NAME_QUERY 0xfcU
/// Specifies to reply to the list of guest names.
#define UART_PROTO_ESC_NAME_REPLY 0xfbU
/// Specifies to delimit the list of guest names.
#define UART_PROTO_ESC_NAME_DELIM 0xfaU

/** @} */

/**
 * @defgroup vcmux_protocol_codes Virtual Channel Mux (VCMux) protocol codes
 * @{
 *
 * VCMux is a transport-agnostic byte-stream multiplexer: multiple logical
 * channels share one physical stream (UART, SSH, TCP) via 0xfe escape framing.
 * This intentionally differs from NVIDIA's 0xff escape so VCMux streams can be
 * carried inside a real NVIDIA TCU stream without collision.
 */
/// Escape byte that begins every VCMux framing sequence.
#define VCMUX_PROTO_ESC_START 0xfeU

/// Escape for a literal 0xfe byte in payload data.
#define VCMUX_PROTO_ESC_ESC   0xfeU

/// Clears the active channel and returns to unframed/default output.
#define VCMUX_PROTO_ESC_DEFAULT 0x00U

/// Switches from channel-select mode to a VCMux control record.
#define VCMUX_PROTO_ESC_CONTROL 0xfdU

/// Announces the complete generated CAmkES stream registry: STREAM_REGISTRY, len_hi, len_lo, JSON bytes.
#define VCMUX_PROTO_CONTROL_STREAM_REGISTRY 0x02U

/// Acknowledges that one complete host-to-target component-stream frame was accepted.
#define VCMUX_PROTO_CONTROL_DOWNLINK_ACK 0x03U

/// virtioso-muxd: a new mux-exec client connected; creates a PTY dynamically.
/// Payload after len_hi/len_lo: [stream_id: u8] [name bytes...]
#define VCMUX_PROTO_CONTROL_STREAM_CONNECTED    0x01U

/// virtioso-muxd: a mux-exec client disconnected; PTY is kept but receives no more data.
/// Payload after len_hi/len_lo: [stream_id: u8]
#define VCMUX_PROTO_CONTROL_STREAM_DISCONNECTED 0x04U

/** @} */

#endif
