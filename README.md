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

    RCE       | 0xe5
    BPMP      | 0xe2
    CCPLEX    | 0xe1
    SCE       | 0xe3
    SPE       | 0xe0
    TZ        | 0xe4

## Virtioso inner mux mode

The Virtioso fork can also demux a nested CAmkES console stream protocol. The
bootstrap/debug mode is enabled with `-V <console-stream-registry.json>`. The
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

The first implementation slice loads PTY names and numeric stream IDs from a
JSON registry at startup. That is only a bootstrap mechanism: the target
architecture is for the muxer to announce generated CAmkES stream IDs and
component names at runtime, then for the demuxer to create PTYs from that live
introspection data.

Runtime announcements use:

    0xfe 0xfd 0x01 <stream-id> <name-len> <name-bytes>

Stream IDs `0`, `0xfd`, and `0xfe` are reserved. Normal payload routing still
uses `0xfe <stream-id>`, and literal payload `0xfe` is escaped as `0xfe 0xfe`.
