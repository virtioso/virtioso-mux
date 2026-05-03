# Orin UARTI Mux Carrier Plan

Updated: 2026-05-03

## Summary

Use Orin AGX's two physical UART paths as two separate architectural channels:

- `ttyACM0` / TCU / CCPLEX stays the raw boot, debug, and recovery channel.
- `ttyACM1` / UARTI becomes the dedicated Virtioso mux/demux carrier.

This avoids implementing NVIDIA TCU/HSP RX before we can log in through the
generated stream model. UARTI is a normal SBSA-style UART at
`/bus@0/serial@31d0000` with GIC INTID `317`, while the TCU path depends on HSP
mailbox receive semantics and conflicts with the existing raw CCPLEX role.

## Verified Current State

- Orin DTS defines UARTI as `/bus@0/serial@31d0000`, physical address
  `0x031d0000`, SPI `285`, GIC INTID `317`.
- `ttyACM0` is already used as the raw CCPLEX path by the Autopilot
  Orin flow.
- VM0 now uses an emulated PL011 console and must receive login input through
  its `serial_getchar` buffer.
- VM1 previously used UARTI passthrough as its proof console. Under this plan,
  VM1 also moves to an emulated PL011 console so UARTI can be owned by native
  CAmkES infrastructure.
- `ConsoleMux` already emits the generated stream registry and `0xfe` stream
  frames. The missing piece was the reverse path from muxed UART bytes to the
  right VM `GetChar` buffer.

## Target Architecture

```text
Host / Autopilot
  ttyACM0 raw_ccplex
    <-> TCU / CCPLEX raw boot and recovery logs

  ttyACM1 mux carrier
    <-> native ConsoleMux on UARTI
        target -> host: 0xfe <stream-id> payload, registry control record
        host -> target: 0xfe <stream-id> payload
        downlink dispatch by generated CAmkES stream registry
        VM stream -> vmN.serial_getchar buffer -> emulated PL011 RX IRQ
```

The mux core remains stream oriented. UARTI is only the Orin physical backend;
it must not become the semantic identity of any VM console.

## Implementation Slices

1. Make `ConsoleMux` bidirectional.
   - Keep `provides Batch mux_batch` for target-to-host stream payloads.
   - Add `provides GetChar getchar` for VM input sinks.
   - Add a generated `demux_sinks` table mapping stream IDs to `GetChar`
     client badges.

2. Move UARTI ownership to `ConsoleMux`.
   - Add Orin UARTI as a platsupport chardev backend.
   - Configure the hardware DTB mapping from `/bus@0/serial@31d0000`.
   - Write mux frames directly to UARTI without newline or ANSI processing.
   - Parse incoming `0xfe` stream frames from UARTI and write bytes into the
     matching VM `serial_getchar` ring buffer.

3. Remove UARTI passthrough from VM1.
   - VM1 uses an emulated PL011 console and joins the generated stream model.
   - UARTI INTID `317` is no longer passed through to VM1.

4. Update Autopilot after target proof.
   - `tty0` remains raw CCPLEX.
   - `tty1` becomes the physical mux carrier.
   - Start host `tcu_muxer` inside each Autopilot run, not at daemon startup,
     so stream PTYs and logs live under that run's `console/console-runtime/`
     directory and never accumulate in a shared runtime directory.
   - Login input must target logical stream names such as `vm0`, not
     `raw_ccplex` and not output-only sinks such as `vm0_guest_console_sink`.

## Validation

Canonical validation remains the Orin AGX clean path:

```bash
make mrproper
make orinagx_defconfig
make vm_qemu_virtio
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain vm-qemu-virtio --binary /home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx --json
```

Proof criteria:

- `ttyACM0` still captures raw CCPLEX boot and recovery output.
- `ttyACM1` receives the target-generated stream registry.
- Host `tcu_muxer` creates logical PTYs/logs from that registry.
- Those PTYs/logs are per-run artifacts under
  `results/<request>/console/console-runtime/`, not daemon-global state.
- Writing `root\n` to the `vm0` logical stream produces a VM0 shell prompt on
  the VM0 output stream.
- VM1 boot/login proof moves from physical UARTI capture to the generated VM1
  stream.

## Risks

- UARTI is no longer an independent VM1 proof console. The raw CCPLEX path is
  retained as the independent recovery channel.
- The Orin UARTI chardev path is intentionally small and assumes the existing
  SBSA/PL011-compatible register model used by the local DT.
- Autopilot must stop treating `tty1` as `VM1`; after this change `tty1` is a
  mux carrier whose logical sessions are announced at runtime.
- A daemon-level `tcu_muxer` makes stream logs cross-run state and should not be
  used for normal validation. If a wrapper is used, it should only preserve the
  physical UART contract and report that the muxer lifecycle is per-run.
