# Virtioso Mux Wire Protocol

This document describes the binary framing used on the UART link between
isengard (Orin AGX) and the Autopilot host, and how `vcmuxer` demultiplexes
the individual streams.  It covers both the CAmkES-side mux and the Linux-side
`virtioso-muxd` mux, which share the same wire format.

## Physical path

```
isengard (seL4 / Linux)                   Autopilot host
  UARTI (0x31d0000)
    ↓ /dev/ttyAMA0  (inside isengard)
    ↓ ttyACM1       (USB-serial on host)
    ↓ vcmuxer -d /dev/ttyACM1
      ├── raw_ccplex PTY    ← all bytes before the Virtual Channel Mux (VCMux) starts
      ├── physical_uart_default PTY
      ├── autopilot_control PTY
      └── <stream name> PTY (one per mux-exec client or CAmkES component)
```

## Layer 0 — NVIDIA outer TCU protocol (0xff)

On `ttyACM0` (CCPLEX primary console) the Jetson firmware wraps each
subsystem's output in NVIDIA's own escape scheme:

| Bytes | Meaning |
|-------|---------|
| `0xff` `<tag>` | Switch active subsystem (CCPLEX=0xe1, BPMP=0xe2, …) |
| `0xff` `0xff` | Literal `0xff` in current subsystem |
| `0xff` `0xfd` | System-level reset |

`vcmuxer` decodes this layer when started with `-O nvidia-tcu -C CCPLEX`.
The CCPLEX payload is then fed to the Virtioso inner layer.

`ttyACM1` (UARTI / ttyAMA0) is **raw** — no NVIDIA outer wrapping. Start
`vcmuxer` with `-O raw` (the default) for this port.

## Layer 1 — Virtual Channel Mux (VCMux) protocol (0xfe / 0xfd)

Both the **CAmkES** mux (seL4 side) and **virtioso-muxd** (Linux side) use the
same escape-sequence framing on the wire.  `0xfe` and `0xfd` are owned by
Virtioso; `0xff` is left for NVIDIA.

### Data frames

To send payload bytes on stream `S`:

```
[0xfe] [S]  [byte0] [byte1] … [byteN]  [0xfe] [0x00]
 ESC   SID  data bytes (0xfe in data    ESC   back-to-default
            encoded as 0xfe 0xfe)
```

The demuxer keeps a "current stream" cursor.  `0xfe S` switches the cursor to
stream `S` and all subsequent bytes (until the next `0xfe`) are routed to that
stream's PTY.  `0xfe 0x00` returns the cursor to the default/unframed state.

Valid stream IDs: **0x01 – 0xfc**.  IDs 0xfd and 0xfe are reserved as protocol
bytes and must not be used as stream IDs.

### Control channel

`0xfe 0xfd` opens a control record.  The next byte is the control type:

| Control type | Value | Format after type byte | Source |
|---|---|---|---|
| `CTRL_CONNECTED`    | `0x01` | `len_hi len_lo stream_id name_bytes…` | virtioso-muxd |
| `STREAM_REGISTRY`   | `0x02` | `len_hi len_lo json_bytes…`           | CAmkES mux |
| `DOWNLINK_ACK`      | `0x03` | *(no payload)*                        | CAmkES mux |
| `CTRL_DISCONNECTED` | `0x04` | `len_hi len_lo stream_id`             | virtioso-muxd |

#### CTRL_CONNECTED (0x01)

Emitted by `virtioso-muxd` when a `virtioso-mux-exec` client connects.
Carries the client's chosen name and the stream ID allocated for it:

```
[0xfe] [0xfd] [0x01] [len_hi] [len_lo] [stream_id] [name bytes…]
```

`vcmuxer` responds by creating a new PTY for `stream_id` with the given
name and emitting a `session_open` JSON record to stdout.

#### CTRL_DISCONNECTED (0x04)

Emitted by `virtioso-muxd` when a client disconnects.  The PTY remains
accessible but will receive no further data.

```
[0xfe] [0xfd] [0x04] [0x00] [0x01] [stream_id]
```

#### STREAM_REGISTRY (0x02)

Emitted by the **CAmkES** mux once, at startup, after the generated stream
list is known.  Carries a JSON blob describing all component streams:

```
[0xfe] [0xfd] [0x02] [len_hi] [len_lo] [json bytes…]
```

JSON format:
```json
{"streams": [
  {"component": "vm0_console", "stream_id": 1, "direction": "output"},
  {"component": "vm1_console", "stream_id": 2, "direction": "output"},
  {"component": "autopilot",   "stream_id": 3, "direction": "bidirectional"}
]}
```

`vcmuxer` applies the registry and creates PTYs for all listed streams.

#### DOWNLINK_ACK (0x03)

Single-byte acknowledgement from isengard that a host-to-target data frame
was received.  No length field follows.

### Escape of 0xfe in data

Any `0xfe` byte appearing in stream payload must be sent as `0xfe 0xfe`.
The demuxer, when in "in-stream" state (after `0xfe S`, before `0xfe 0x00`),
interprets `0xfe 0xfe` as a single literal `0xfe` byte.

## vcmuxer modes

`vcmuxer` is the C binary at `sources/tcu_muxer/`.

| Flag | Effect |
|------|--------|
| *(none)* | Legacy NVIDIA TCU outer-only mode; one PTY per subsystem tag |
| `-A` | Virtual Channel Mux (VCMux) mode; reads STREAM_REGISTRY or CTRL_CONNECTED to build the PTY table dynamically |
| `-O raw` | Outer layer is raw (default); use for ttyACM1 / ttyAMA0 |
| `-O nvidia-tcu -C CCPLEX` | Outer layer is NVIDIA TCU; use for ttyACM0 |
| `-s <dir>` | Write per-stream log files to `<dir>/<name>.txt` |
| `-d <dev>` | UART device (default `/dev/ttyUSB3`) |

Typical invocation for the UARTI / `virtioso-muxd` path:

```bash
vcmuxer -A -O raw -d /dev/ttyACM1 -s /tmp/mux-streams
```

### PTY session records

In `-A` mode `vcmuxer` emits one JSON line to stdout per PTY created:

```json
{"event":"session_open","name":"zenoh_demo","kind":"camkes_component_stream",
 "stream_id":1,"pty_path":"/dev/pts/7","log_path":"/tmp/mux-streams/zenoh_demo.txt"}
```

Autopilot's `setup_demo` step parses these lines to resolve `mux:<name>` pane
sources to the correct PTY path.

### Built-in PTYs (always present in -A mode)

| Name | Index | Purpose |
|------|-------|---------|
| `raw_ccplex` | 0 | All bytes before the inner mux starts (kernel boot, noise) |
| `autopilot_control` | 1 | Bidirectional autopilot control channel |
| `physical_uart_default` | 2 | Bytes received while no stream is active |

## virtioso-muxd

`virtioso-muxd` is the Rust binary at `sources/virtioso-muxd/`.  It runs
**inside isengard** (on the target) and multiplexes named Unix-socket clients
onto a single sink (UART, file, or TCP server).

```
virtioso-mux-exec --name zenoh_demo -- sh -c 'isengard-demo-zenoh-remote ...'
   ↓ Unix socket /run/virtioso-mux/control.sock
virtioso-muxd
   ↓ --sink uart:/dev/ttyAMA0   (or file: or tcp:)
vcmuxer on host
```

### Stream lifecycle

1. `virtioso-mux-exec` connects to the Unix socket, sends `[name_len][name]`.
2. `virtioso-muxd` allocates a stream ID (1–0xfc, skipping 0xfd and 0xfe),
   replies with the ID byte.
3. Sends `CTRL_CONNECTED` control frame on the wire.
4. Forwards all data from the client as data frames on the assigned stream ID.
5. On client disconnect, sends `CTRL_DISCONNECTED`.

### Sink types

| Spec | Behaviour |
|------|-----------|
| `uart:/dev/ttyAMA0` | Write frames to the serial device |
| `file:/tmp/mux.bin` | Write frames to a file (create/truncate) |
| `tcp:0.0.0.0:PORT` | Listen for one TCP connection (demuxer connects) |

TCP mode is useful when the host and target are on the same LAN and you want
to avoid sharing the UART with the Autopilot console reader.

## Protocol comparison

| Aspect | CAmkES mux | virtioso-muxd |
|--------|-----------|---------------|
| Wire framing | Escape-sequence (0xfe/0xfd) | **Same** |
| Stream discovery | `STREAM_REGISTRY` JSON at boot | `CTRL_CONNECTED` per client |
| Stream IDs | Fixed at compile time | Dynamically allocated |
| Downlink (host→target) | Supported with ACK | Not used in current demos |
| Carries kernel console | Mixed in before registry | Mixed in before first client |

## Adding a new stream name in virtioso-mux-exec

```bash
virtioso-mux-exec --name my_stream -- my_program args...
```

`vcmuxer` will create `/dev/pts/N` and write to `/tmp/mux-streams/my_stream.txt`
as soon as the CTRL_CONNECTED frame arrives.  No restart of vcmuxer needed.
