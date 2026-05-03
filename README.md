# tcu_muxer

## ABOUT

Tegra Combined UART generates a pseudoterminal for communication for each UART
client on Tegra. It handles the tag protocol used to communicate with specific
UART clients. This is handled in tcu_com.c.

---

---

## DESCRIPTION

tcu_com.c (tegra combined uart com) This is the main program. It takes the
argument -d /dev/ttyUSB3 and creates pseudoterminals for each combined UART
client. It handles the tag communication protocol below:

### CLIENT TAG

    CCPLEX       | 0xe1
    BPMP         | 0xe2
    OOBHUB       | 0xe3
    SatMC        | 0xe4
    RAS          | 0xe5
    CCPLEX-ROOT  | 0xe6
    TZ           | 0xe7
    CCPLEX-REALM | 0xe8
    CCPLEX-ALT   | 0xe9
    MSEQ         | 0xea
    PCORE        | 0xeb
    C2C          | 0xec
    DBG2         | 0xed
    RSVD15       | 0xef
    RSVD16       | 0xf0

`PSC` is accepted as a compatibility alias for `CCPLEX` because the Orin AGX
CPU/UEFI/seL4 console payload is observed on tag `0xe1`.

## Virtioso inner mux mode

The Virtioso fork can also demux a nested CAmkES console stream protocol. The
runtime-introspection mode is enabled with `-A`.

The NVIDIA TCU protocol remains the outer protocol and keeps using `0xff`.
Virtioso streams use an inner escape byte, `0xfe`, so the nested stream can be
carried through a real NVIDIA TCU path without colliding with NVIDIA framing.

Outer modes:

    -O raw         input bytes are already the Virtioso 0xfe stream
    -O nvidia-tcu input bytes are a real NVIDIA TCU stream

When `-O nvidia-tcu` is selected, `-C <tag>` chooses the NVIDIA client carrying
Virtioso traffic. The default is `CCPLEX`.

When the input device is a router-managed pseudoterminal rather than a real
UART, pass `-L` to disable UUCP lock-file handling.

In Virtioso mode, the muxer always creates three stable channels before any
component stream PTYs:

- `raw_ccplex` is the stable raw CCPLEX channel. It is opened immediately and
  carries decoded CCPLEX payload bytes in both directions. On Orin AGX,
  Autopilot should use this PTY for UEFI, elfloader, and fallback diagnostics
  instead of opening `/dev/ttyACM0` directly.
- `autopilot_control` is the Autopilot-to-muxer control/introspection channel.
  It is not the guest console and it does not mirror the raw input stream.
- `physical_uart_default` is the unframed physical UART default stream. It
  carries VM0/driver-VM output when no generated component stream is selected.
  It is muxer-created transport state, not a generated CAmkES component stream.

The `-A` runtime path waits for the seL4-side `ConsoleMux` to send the complete
generated CAmkES stream registry over the mux control stream. Autopilot does
not pass this registry to `tcu_muxer`; the registry is target-originated
runtime introspection data.

The registry control record uses:

    0xfe 0xfd 0x02 <json-len-hi> <json-len-lo> <json-bytes>

Stream IDs `0`, `0xfd`, and `0xfe` are reserved. Normal payload routing still
uses `0xfe <stream-id>`, and literal payload `0xfe` is escaped as `0xfe 0xfe`.
`0xfe 0x00` clears the active generated component stream and returns following
unframed bytes to `physical_uart_default`.

When the registry arrives, `tcu_muxer` first reports it on stdout as a
`stream_registry` event, then opens the component PTYs described by the
registry:

    {"event":"stream_registry","registry_json":"{...}"}

In Virtioso mode, each PTY creation is reported on stdout as a JSON
`session_open` record. For generated component streams, the `name` and
`stream_id` fields come from the target-provided registry:

    {"event":"session_open","name":"vm1_guest_console_sink","kind":"camkes_component_stream","stream_id":2,"pty_path":"/dev/pts/19","log_path":".../vm1_guest_console_sink.txt"}
