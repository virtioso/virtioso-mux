# Cross-Arch TCU-Style UART Mux Plan

Date: 2026-05-01

Status: active architecture plan

Primary goal: finish the CAmkES component mux/demux path on real Orin AGX
first, while keeping the protocol and component identity model reusable across
architectures. The `qemu_x86_64` path is now backlog/regression work, not the
active completion target.

This plan promotes the x86 console-mux work into a cross-architecture topic,
but the current execution priority is Orin AGX. x86 remains useful because it
started the stream-registry and demux work, but x86-specific completion should
not block finishing the Orin CAmkES component mux/demux path.

Related notes:

- [../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md](../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md)
- [../platforms/orin-agx/investigations/tcu-console-sporadic-hang.md](../platforms/orin-agx/investigations/tcu-console-sporadic-hang.md)

Reference implementation:

- `sources/tcu_muxer/uart-proto.h`
- `sources/tcu_muxer/tcu_com.c`

## Summary

The current x86 work proves that named console streams are needed, but its
`CF` / `binary_frames` implementation is the wrong long-term transport and x86
bring-up is not the right critical path for finishing mux/demux. The Arm/Orin
side already has NVIDIA TCU as VM0's Linux console and UARTI passthrough as
VM1's physical console, while the in-VMM PL011 emulator is only a simple output
path.

The recommended direction is one shared, tiny TCU-style mux protocol above the
producer boundary, with platform-specific UART attachment below it. It is not
wire-compatible with NVIDIA `vcmuxer`: our fork uses `0xfe` as its escape
byte so it can run on top of NVIDIA's real TCU stream without conflicting with
NVIDIA's own `0xff` escape byte. x86 should use the current
`GuestConsoleSink`/`ConsolePassthroughSink` work as an implementation
substrate, but the first milestone is not byte encoding. The first milestone is
build-time generation of the stream registry and component-to-stream mappings,
because every later encoder, demuxer, PTY, and Autopilot channel must consume
that registry instead of carrying manually assigned IDs.

## Current State

### Common Protocol Baseline

NVIDIA `vcmuxer` provides the behavior shape to copy, but not the exact
escape byte:

- NVIDIA uses `0xff` as its escape byte.
- Our mux protocol uses `0xfe` as its escape byte.
- `0xfe 0xfe` carries a literal payload byte `0xfe`.
- `0xfe <id>` switches the active stream.
- There is no default component stream. Any reset/resync marker must leave the
  demuxer with no active component stream until the mux announces or selects a
  generated stream again.

The `0xfe` choice is intentional. It allows our muxed stream to travel through
or alongside NVIDIA's real TCU tooling without making our escape byte
ambiguous with NVIDIA `vcmuxer` framing.

The required part for this workspace is generated stream identity,
introspection, stream-switching, and byte escaping. NVIDIA's guest-name
query/reply mechanism is not the desired metadata path; the mux should announce
the generated CAmkES stream registry directly.

### x86 `qemu_x86_64` Work To Move Into This Topic

Current x86 `vm_qemu_virtio` work already established the right stream model:

- `driver_vm_console` is VM0 login and shell automation
- `user_vm_console` is the intended VM1 console
- `vmm_mux_control` is legacy VM-prefixed mux/control traffic
- `vmm_debug` is explicit VMM debug traffic and must stay distinct from
  `vmm_mux_control`

Verified implementation pieces:

- `tools/qemu_runner.py`
  declares the logical stream IDs used by the host runtime.
- `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`
  previously hard-coded target-side stream IDs for VM0 console, VM1 console,
  and VMM/infra diagnostics. These app assignments are now migration history,
  not the target contract.
- `components/GuestConsoleSink/src/guest_console_sink.c`
  now prefixes non-empty batches with `0xfe <generated-stream-id>`, escapes
  literal payload `0xfe` bytes as `0xfe 0xfe`, gets the stream ID from
  generated CAmkES identity, and flushes on newline, carriage return, prompt
  colon, or threshold.
- `components/ConsolePassthroughSink/src/console_passthrough_sink.c`
  is the raw batch-to-physical-serial sink.
- `components/ConsoleMux/src/console_mux.c`
  exists in the minimal x86 path and forwards batched bytes to an uplink, but
  its optional report path now uses generated CAmkES identity and suppresses
  reports when the mux itself has no valid stream.
- `tools/console_router.py`
  no longer carries the old `CF`, JSONL, binary-frame, or `line_prefixes`
  compatibility paths. Until the real demuxer lands, the host runner uses a
  single `process_stdio` channel.

This work should not remain an x86-only branch of the architecture. The useful
parts are the current need for distinct stream identities, CAmkES producer
connections, batch sink work, and host runtime channel model. The hard-coded
IDs listed above are migration liabilities, not a contract to preserve. The old
transport implementations are not compatibility constraints: `CF`,
`binary_frames`, `jsonl_frames` as a mux transport, and `line_prefixes` should
be dropped from the mux/demux target rather than maintained as fallback
behavior. They exist only in local post-upstream work and are safe to rewrite.
The first removal slice is complete for active x86 host tooling and VMM console
transport: the old frame decoder/encoder tools are gone, and
`projects/vm/components/Init/src/console_frame_transport.*` emits raw bytes to
the per-component sink instead of assigning VMM stream IDs itself.

### Arm / Orin AGX State

Current Orin AGX `vm_qemu_virtio` console wiring is not the same as x86:

- VM0 Linux uses NVIDIA TCU as `/serial` with `console=ttyTCU0`.
- The TCU node is generated in
  `src/plat/orinagx/fdt.c`
  only when the HSP nodes needed by the TCU mailbox path are present.
- VM0 passes HSP Top0/AON, GPIO, SDMMC4, MGBE, SMMU, BPMP-related nodes, and
  TCU-related IRQs to Linux in
  `apps/Arm/vm_qemu_virtio/orinagx/devices.camkes`.
- VM1 Linux currently uses UARTI passthrough at `/bus@0/serial@31d0000` with
  `console=ttyAMA0,115200n8`; its `dtb_irqs` includes INTID `317`, UARTI SPI
  `285`.
- `vm1.pl011 = 0x9000000` remains configured only to keep the existing VMM
  module present while UARTI passthrough is isolated.

The PL011 emulator is not an input-capable console today. Its write handler
only forwards guest UARTDR writes to host `putchar()`, and the read handler
only reports transmitter-ready state. That makes it a useful first producer for
TCU-style muxed output, but not a full interactive replacement for UARTI yet.

The Orin TCU path also has a historical risk: the TCU console can hang around
the Linux `ttyTCU0` switch and BPMP probe. That makes TCU useful for VM0 today,
but not a reason to route all VM1 progress through physical TCU before we have
a muxed fallback path.

## Recommendations

### 1. Implement Template-Generated Stream Mappings First

Rationale:
The prior plans repeatedly left stream generation as a later cleanup item. That
ordering is now wrong. The generated identity registry is the architectural
foundation: it is the only allowed source of component names, component types,
console-stream eligibility, directions, and host-visible metadata. Numeric
stream IDs are generated for eligible components by common CAmkES template code,
not by hand-authored app configuration.

Affected areas:

- CAmkES/Jinja2 templates under `projects/virtioso-camkes-vm/templates`
- `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`
- `apps/Arm/vm_qemu_virtio/*/devices.camkes`
- `tools/qemu_runner.py`
- `tools/console_router.py` or its replacement demux tool
- generated stream metadata consumed by Autopilot

Short-term benefit:
The first mergeable implementation slice proves that the composition can
enumerate streams automatically before any mux byte protocol is relied on.

Migration implication:
Do not preserve the current numeric IDs even temporarily as hand-authored
values. All components should have generated identity metadata. Components
without UART-like console capability should carry an invalid stream ID such as
`-1`; only eligible console components receive valid stream IDs from a generated
counter.

Generated artifacts should include at least:

- common generated component identity accessors, such as instance name,
  component type, and console stream ID
- a target-side identity table that the mux can announce at runtime
- a host-side generated metadata file for inspection and build/test tooling
- enough metadata to map each valid stream back to its CAmkES component name,
  type, direction, and selected endpoint capability

### 2. Make Manual Stream IDs Impossible

Rationale:
The target architecture has no manually assigned stream IDs. A component,
application, host tool, demuxer, or Autopilot chain must not carry its own
numeric stream map. The only valid numeric IDs are those emitted by generated
CAmkES template code for UART-like components; other components report `-1`.

Affected areas:

- CAmkES component attributes such as `stream_id`
- mux emitter interfaces that accept raw integer stream IDs from application
  code
- generated headers and generated CAmkES glue
- `components/GuestConsoleSink/src/guest_console_sink.c`
- `components/ConsoleMux/src/console_mux.c`
- `projects/vm/components/Init/src/console_frame_transport.h`
- host tools that currently embed stream IDs

Short-term benefit:
Accidental hardcoding becomes a build failure instead of a review burden.

Migration implication:
Remove or hide public APIs that let application code provide a stream ID. The
preferred shape is that common generated CAmkES component code exposes
`get_instance_name()` plus a generated stream-ID accessor that returns `-1` for
non-console components. Hand-written code should only emit bytes to "my
console stream" and should not know how the numeric ID was chosen. If any
`stream_id = ...` attribute remains in app CAmkES files, or if host-side code
defines a numeric stream table, treat that as a failed migration.

### 3. Give Every CAmkES Component Identity, Not Always A Stream

Rationale:
There is no default stream that multiple components read from or write to by
default. Every CAmkES component should have generated identity metadata, but
only components with UART-like I/O capability should receive a valid stream ID.
Non-console components should report an invalid stream ID, for example `-1`.
Shared presentation views can be derived later, but the wire/runtime identity
for console-bearing components remains component-specific.

Affected areas:

- VM components and VMM support components that currently share putchar-style
  paths
- `GuestConsoleSink`, `ConsolePassthroughSink`, `ConsoleMux`, and diagnostic
  producers
- generated CAmkES connection templates
- common CAmkES component templates such as `component.common.c`
- demux presentation and logging policy

Short-term benefit:
The registry can explain exactly which component produced a byte stream. That
removes the ambiguity that made `line_prefixes` and grouped debug streams
fragile.

Migration implication:
The older named stream taxonomy remains useful as a view, not as the identity
model. For example, `driver_vm_console` and `user_vm_console` can remain stable
logical labels, but each label is generated from the owning component instead
of from a hand-written stream number.

Initial stream eligibility should be conservative:

- valid stream ID for components with `uses PutChar`
- valid stream ID for components with `provides PutChar`
- valid stream ID for components with `uses GetChar`
- valid stream ID for components with `provides GetChar`
- invalid stream ID for all other components by default

`Batch` is intentionally not part of the first eligibility rule because it is a
generic transport shape, not inherently a UART-like console capability. We may
need to include `Batch` later for mux-specific batch endpoints, but that should
be done with an explicit console-bearing annotation or connector rule rather
than treating every `Batch` endpoint as a stream.

### 4. Add Mux/Demux Introspection

Rationale:
The demuxer must not contain hard-coded stream IDs or PTY mappings. When the
muxer and demuxer connect, the muxer announces the generated registry. The
demuxer creates logs, PTYs, and host-visible names from that announcement.

The announcement must include at least:

- stream ID
- CAmkES component name
- CAmkES component type
- direction and capability metadata
- optional stable aliases for tools that need names such as VM0 console or VM1
  console

The forked demuxer must support two upstream UART shapes:

- NVIDIA `vcmuxer` is already between the real TCU UART and our demuxer. In
  this mode, consume the stream already exposed by NVIDIA's tool and apply only
  our `0xfe` mux rules.
- NVIDIA `vcmuxer` is not present. In this mode, our demuxer must also do
  the direct input/output work NVIDIA `vcmuxer` normally performs, following
  the behavior in `sources/tcu_muxer/tcu_com.c`, while still using `0xfe` for
  our nested stream switching.

Initial implementation:
`sources/tcu_muxer` now has a Virtioso mode selected by
`-V <console-stream-registry.json>`. In that mode, NVIDIA `0xff` TCU handling
stays an outer protocol selected with `-O nvidia-tcu -C <tag>`, while
`-O raw` treats input as an already-selected Virtioso byte stream. The inner
parser uses `0xfe <stream-id>` stream switches and `0xfe 0xfe` literal escapes
and creates PTYs from runtime-loaded stream names. The JSON loader is a
bootstrap/debug path only. The live path uses `-A`, starts with only the RAW
PTY, then creates stream PTYs dynamically from target announcements:
`0xfe 0xfd 0x01 <stream-id> <name-len> <name-bytes>`. `GuestConsoleSink` emits
that announcement before its first payload batch. Stream IDs `0`, `0xfd`, and
`0xfe` are reserved and must not be assigned to CAmkES streams.

Affected areas:

- `tools/console_router.py` or its replacement
- possible new `tools/tcu_console_router.py` or a small adapter around
  `sources/tcu_muxer`
- mux startup/reset behavior
- generated runtime registry table
- PTY and log directory creation

Short-term benefit:
The host discovers stream IDs and PTY names from the running image, not from a
parallel Python table. This also gives Autopilot one source of truth for
runtime channels.

Migration implication:
Use the latest seL4 community upstream commit as the compatibility boundary.
Mux/demux work after that point is local-only and may be rewritten. The
replacement still needs Orin AGX proof before calling the new path complete.
The x86 `qemu_x86_64_defconfig` proof for VM0 login and VM1/user-VM readiness
is backlog/regression work after Orin has proven the mux/demux path.

### 5. Move Autopilot And Orin UART Selection To Introspection

Rationale:
Autopilot must not know hard-coded stream IDs or hard-coded logical `/dev`
nodes such as `tty0`/`tty1` for post-demux channels. It should consume the
demuxer's introspection output: component names, stable aliases, PTY paths, log
paths, and capabilities.

The Orin AGX `ttyACM0`/`ttyACM1` issue has a separate bootstrap boundary. Some
piece of platform code still has to find the physical serial transport before
the mux handshake can happen. That mapping should move out of chain logic and
into platform inventory or daemon-side discovery. After the muxer connects,
Autopilot should switch to introspected logical streams instead of assuming
`tty0` is VM0 and `tty1` is VM1.

Affected areas:

- Autopilot chain definitions and request context
- Orin AGX restart/start command policy
- demux introspection output format
- result collection and analysis hooks that read `console/tty0.raw` or
  `console/tty1.raw`

Short-term benefit:
The same identity path serves interactive PTYs, logs, and automated
expectations. Host paths become runtime facts, not duplicated configuration.

Migration implication:
Do not remove the existing physical Orin UART proof path until the platform
inventory and mux introspection path can reproduce it. The plan should define
where the unavoidable physical-device bootstrap data lives before deleting the
current `ttyACM0`/`ttyACM1` usage from operational docs.

### 6. Gate Mux/Demux With Default-On Kconfig

Rationale:
Applications need a clear switch for "use the new mux/demux path" versus
"behave like upstream". That switch should be controlled by a new Kconfig item,
default `y`, and then consumed by app-specific `settings.cmake`.

Affected areas:

- `virtioso-build/Kconfig` or a sourced Virtioso Kconfig fragment
- `projects/virtioso-camkes-vm/settings.cmake`
- app-specific `settings.cmake` / `app_settings.cmake`
- CAmkES templates that include or omit mux components
- host tooling that expects demux introspection

Short-term benefit:
The default path exercises mux/demux in normal builds, while a deliberate
off-switch preserves upstream-style behavior for comparison and bisecting.

Migration implication:
Define the off mode precisely per application. In `virtioso-camkes-vm`, "off"
means no new mux/demux components, no generated mux-only wiring requirement,
and console behavior equivalent to the relevant upstream CAmkES VM path. If an
application has ambiguous upstream behavior, resolve that ambiguity before
using it as the off-mode test oracle.

### 7. Replace `CF` Emission With `0xfe` TCU-Style Escaped Bytes

Rationale:
After generated stream identity exists, the target side should emit only
registry/introspection records, stream switches, and payload bytes. Use `0xfe`
as the escape byte, not NVIDIA's `0xff`, so this mux can run even when the
underlying hardware path is NVIDIA TCU. Finish this path on Orin AGX first;
the x86 proof becomes a later regression/backlog item.

Affected areas:

- `components/GuestConsoleSink/src/guest_console_sink.c`
- `components/ConsoleMux/src/console_mux.c`
- `components/ConsolePassthroughSink/src/console_passthrough_sink.c`
- generated per-component emit wrappers
- x86 and Arm CAmkES wiring selected by Kconfig

Short-term benefit:
Orin AGX gets a real component mux/demux path only after stream ownership and
host metadata are no longer hand-maintained. x86 can reuse the same transport
later, but does not drive the active sequencing.

Migration implication:
Profiling/debug records must stop being injected as transport frames. Route
them to their generated component streams or leave them as build-time debug
output.

Implementation note:
This slice has been started on x86: `GuestConsoleSink` and the optional
`ConsoleMux` report path now use the `0xfe` escape/switch encoding, the VMM
console transport no longer emits `CF` records or owns stream IDs, and
`console_router.py` / `qemu_runner.py` no longer expose the old binary, JSONL,
or line-prefix transport modes. The remaining work is the real `0xfe` demuxer
with runtime registry/introspection and PTY creation.

### 8. Add Hardware UART Backend Interfaces

Rationale:
The muxer interfaces with a real UART on hardware platforms. The mux protocol
is shared, but UART ownership, MMIO layout, IRQ handling, clock/reset
requirements, and device-tree description are platform/backend concerns. Arm
must be able to support UART device passthrough using the same kind of device
tree-driven hardware description used by VM passthrough.

Backend shape:

- a mux core that owns stream encoding, stream registry announcement, and RX/TX
  policy
- a platform UART backend that owns the physical UART driver and hardware
  attachment
- backend selection from platform/app configuration, not hard-coded in the mux
  core
- shared backend implementations when platforms use compatible UART blocks,
  for example SBSA UART, PL011, 8250-compatible UART, or Tegra HSUART where
  applicable
- Arm backends that can consume generated or passthrough-style device-tree
  metadata for MMIO regions, IRQs, clocks/resets where applicable, and
  passthrough ownership decisions

Affected areas:

- platform-specific CAmkES device descriptions and `devices.camkes`
- Arm device-tree generation and passthrough metadata
- UART driver/backend code shared across platforms where possible
- x86 QEMU serial/chardev backend code
- Kconfig and app settings that select the mux path and backend

Short-term benefit:
The mux core remains portable while hardware integration stays explicit. Orin,
qemu-arm-virt, qemu-x86, and future platforms can share the stream protocol
without pretending they share UART hardware.

Migration implication:
Do not bake Orin TCU, UARTI, SBSA UART, PL011, or QEMU chardev assumptions into
the mux core. Add a backend boundary before replacing platform UART wiring.

### 9. Provide A Bidirectional Arm Guest UART Device Model

Rationale:
Arm VMs need a real guest-facing console device, not just dumb earlycon output.
The current `virtioso-camkes-vm` shape already exposes PL011-like guest UART
emulation, so PL011 is the default target interface unless another UART device
model is clearly better for a platform. Whatever device is chosen must support
both TX and RX so Linux can use it as a normal console and interactive login
path.

Device-model shape:

- guest sees a standard UART device such as PL011, or a similarly well-supported
  UART model
- TX from the guest enters the generated stream/mux path
- RX from the demux/input side is injected into the guest UART model with proper
  interrupt/status behavior
- device-tree presented to the guest describes the emulated UART, not an
  accidental transport implementation detail
- the backend contract should be close enough to QEMU's UART device model that
  QEMU can act as the device-model backend when the same VM is executed on
  "bare metal" or outside the seL4 VMM path

Affected areas:

- `src/camkes/modules/pl011.c`
- `templates/pl011.template.c`
- Arm VM CAmkES connections or generated hooks from the guest UART model into
  the shared mux encoder and RX injection path
- qemu-arm-virt, rpi4, and Orin VM1 PL011 configurations
- guest DTB generation for the selected virtual UART
- possible QEMU backend/device-model adapter if the VM is run outside seL4

Short-term benefit:
Arm gets a proper muxed VM console path that can eventually replace physical
UARTI passthrough for interactive console use, not only for printk/earlycon
capture.

Migration implication:
The existing PL011 code is only a starting point because it is currently
output-only. Keep UARTI passthrough as the Orin VM1 interactive/proof console
until the guest UART model supports RX, status, and interrupt behavior well
enough for normal Linux console operation.

### 10. Add UART-Like TX Buffering At Guest-Facing Sinks

Rationale:
Guest-facing UART sinks should behave more like a 16450/16550-style UART
transmit path than like one CAmkES RPC per byte. The guest can still write
characters one at a time, but the VMM-side sink should coalesce them before
calling the mux/uplink path. This keeps the guest-visible model simple while
reducing CAmkES RPC pressure on the muxer and physical UART backend.

Buffering policy:

- each guest-facing UART sink owns a per-stream TX staging buffer
- newline or carriage return flushes the staged bytes immediately
- a full staging buffer flushes immediately
- on the first byte of a non-empty buffer, schedule a delayed flush; use a
  configurable timeout, with `50 ms` as the initial default
- if more guest bytes arrive before the delayed flush fires, keep appending
  until newline, carriage return, or capacity triggers an earlier flush
- when the delayed flush fires, send whatever is staged even if no newline has
  arrived
- after scheduling the delayed flush, return from the guest write path so the
  VMM can continue/yield instead of blocking for a synchronous UART drain
- the flush path sends one batch to the muxer/real UART backend for that
  component stream

Affected areas:

- `components/GuestConsoleSink/src/guest_console_sink.c`
- the future Arm PL011 or selected guest UART model
- `projects/vm/components/Init/src/serial.c` and related guest UART emulation
  paths where x86 guest writes enter the console stream
- timer integration in the VMM or sink component; the existing `Init` timer
  path is a useful reference, but the final owner should be the guest-facing
  UART sink/model, not the mux core
- `Batch` or `seL4SerialServer` connectors only as transport mechanisms; they
  should not define buffering semantics themselves

Short-term benefit:
The muxer sees larger, line-oriented or timeout-oriented writes instead of a
stream of tiny RPCs. This should reduce overhead while preserving interactive
console behavior for prompts and partial lines.

Migration implication:
Do not push this policy into the host demuxer or physical UART backend. The
backend should receive already-coalesced bytes for a selected stream. The mux
core can stay small: stream switch, byte escaping, introspection, and UART
backend IO. Buffering belongs at the producer side where guest UART semantics
are known.

### 11. Preserve Platform-Specific Physical UART Policy

Rationale:
x86 QEMU can use a dedicated second serial/chardev uplink. Orin has real TCU,
UARTI, HSP, BPMP, and SCR/firewall constraints. The shared architecture should
be the generated stream registry, mux encoder, introspection, demux behavior,
and UART backend interface, not a forced identical physical UART topology.

Affected areas:

- x86 QEMU runner `-serial` / dedicated mux chardev handling
- Orin `devices.camkes` TCU/UARTI device allocation
- Arm passthrough-style device-tree metadata for mux-owned UARTs
- shared SBSA/PL011/8250/Tegra UART backend candidates
- Autopilot platform source discovery

Short-term benefit:
The plan can progress on Orin without destabilizing the fixed UARTI proof path.

Migration implication:
The Orin target should choose one of two explicit modes:
`physical-uarti-console` for direct VM1 hardware console, or
`muxed-pl011-console` for VM1 VMM-emulated output. Do not blend them silently.
For the current Orin-first mux work, keep UARTI as the known-good bootstrap and
proof path while completing the muxed PL011/component-stream path alongside it.

### 12. Require Clean Repos, `before-mux` Branches, And Frequent Commits

Rationale:
This change crosses multiple repos and intentionally rewrites local mux/demux
work. Before implementation starts, every affected repo must have current work
committed, and a `before-mux` backup branch must be created from the committed
state.

Affected areas:

- `projects/virtioso-camkes-vm`
- any touched source repo under `sources/`
- `/home/hlyytine/autopilot` before Autopilot changes
- any Yocto layer or VM-image repo touched by follow-up implementation

Short-term benefit:
The rewrite has clear rollback points and reviewable commit boundaries.

Migration implication:
Implementation should proceed in small commits: registry generation first,
compile-fail enforcement second, introspection third, UART backend boundary
fourth, encoder/demux fifth, and Autopilot consumption only after host
introspection exists.

## Validation Path

### Phase 0: Repository And Backup Gate

1. Identify every repo that will be touched.
2. Confirm each repo has no uncommitted work except explicitly approved
   pre-existing changes.
3. Commit current work in the appropriate repos before implementation.
4. Create a `before-mux` branch in each touched repo from that committed state.
5. Do not start mux implementation commits until this gate is complete.

### Phase 1: Generated Registry Proof

1. Build a target app and prove the generated component identity registry exists
   before any byte-encoder changes are relied on.
2. Confirm every CAmkES component has generated identity metadata.
3. Confirm every CAmkES component without UART-like console capability reports
   an invalid stream ID, for example `-1`.
4. Confirm every CAmkES component with `PutChar` or `GetChar` capability gets a
   valid generated stream ID.
5. Confirm `Batch` endpoints are not automatically treated as console streams;
   document any mux-specific batch endpoint that will need explicit stream
   eligibility later.
6. Confirm no app CAmkES file manually assigns a stream ID.
7. Confirm hand-written host tools do not define numeric stream IDs.
8. Confirm generated host metadata contains component names, types, directions, and
   optional stable aliases.

### Phase 2: Compile-Fail Enforcement

1. Remove or hide `stream_id` CAmkES attributes from hand-authored app code.
2. Remove public APIs that take raw stream IDs from ordinary component code.
3. Add checks that fail the build if application CAmkES or host tooling tries
   to define manual stream IDs.
4. Keep generated stream IDs private to generated code and runtime identity
   metadata.

### Phase 3: Introspection And Demux Proof

1. Start the mux and demux with no hard-coded stream table on the demux side.
2. Confirm the mux announces stream IDs, CAmkES component names, directions,
   and aliases from the generated registry.
3. Confirm the demux creates PTYs and logs from that announcement.
4. Confirm the demuxer works when NVIDIA `vcmuxer` is already present and
   when our demuxer directly handles the real TCU UART path.
5. Confirm no active path depends on `CF`, `binary_frames`, or `line_prefixes`.

### Phase 4: Orin AGX Mux/Demux Proof

This is now the active completion target. x86 reached enough evidence to prove
the demux runtime can start, but x86-specific VM control faults should not hold
the mux/demux work hostage.

1. Clean build from workspace root:
   `make mrproper`, `make orinagx_defconfig`, `make vm_qemu_virtio`.
2. Submit through Autopilot using the canonical Orin AGX EFI chain.
3. Preserve the known-good UARTI VM1 proof: VM1 output should still appear via
   the platform-discovered UARTI source with `/bus@0/serial@31d0000` and INTID
   `317`.
4. Route at least one Orin CAmkES component stream through the generated
   stream ID path and `0xfe` mux encoder.
5. Confirm the demux creates named PTYs/logs from runtime announcements rather
   than from a static Python table.
6. Add the bidirectional guest UART proof separately: expose PL011 or the
   selected guest UART model to Linux, route guest TX into an introspected
   demuxed channel, inject RX from the demux/input side, and prove normal Linux
   console interaction rather than only earlycon output.
7. Confirm the Arm guest UART TX path uses the same producer-side buffering
   policy: newline flush, capacity flush, and a configurable delayed flush with
   `50 ms` as the starting default.
8. Prove Autopilot consumes logical channel introspection rather than
   hard-coded `tty0`/`tty1` stream assumptions.
9. Only after that, evaluate whether UARTI remains useful as a separate
   physical console or can be replaced by the muxed guest UART path.

### Phase 5: x86 Backlog / Regression Proof

Initial implementation:
`tools/console_router.py` now supports `transport.type = "virtioso_tcu_mux"`.
For `vm_qemu_virtio` profiles, `tools/qemu_runner.py` writes a local runner
manifest that starts `sources/tcu_muxer/vcmuxer -A` and records dynamic
PTY/log sessions from stream announcements. Remote bundles deliberately keep
their inner manifest as `process_stdio`; the local runner-side router demuxes
the SSH stream so we do not run two demuxers in series.

Real-run evidence from 2026-05-01:

- `qemu_x86_64_defconfig` clean build completed with `make mrproper`,
  `make qemu_x86_64_defconfig`, and `make vm_qemu_virtio`.
- Autopilot request `20260501-175337` used `transport.type =
  "virtioso_tcu_mux"` and created
  `console/console-runtime/sessions.json`.
- The router now mirrors the demuxer's `RAW` log back to stdout so the legacy
  `tty0.raw` watcher still sees early boot text while Autopilot is being moved
  to introspected sessions.
- No `0xfe 0xfd` stream announcements reached the demuxer in that run. The
  target stopped before guest console sinks could announce themselves, with
  `X86EPTPageMap: Need a page directory first.` from `vm0:control`.

Backlog rule:
do not fix the x86 VM control EPT fault merely to complete mux/demux. Return to
this path after the Orin AGX mux/demux path proves the generated stream
registry, runtime announcements, demux-created channels, and Autopilot
introspection flow.

1. Clean build from workspace root:
   `make mrproper`, `make qemu_x86_64_defconfig`, `make vm_qemu_virtio`.
2. Submit through Autopilot using the canonical
   `qemu_x86_64_defconfig` chain.
3. Confirm the `0xfe` demux creates named channels from introspection rather
   than from a Python table.
4. Confirm VM0 login/shell automation uses the introspected driver-VM console
   alias or component name, not `tty0`.
5. Confirm guest-facing sinks coalesce output: newline flushes immediately,
   partial lines flush after the configured timeout, and byte-at-a-time guest
   writes do not cause byte-at-a-time mux/uplink RPCs.
6. Confirm no hard-coded stream IDs remain in target, demux, or Autopilot code.

### Phase 6: Hardware UART Backend Proof

1. Prove the mux core builds without platform-specific UART assumptions.
2. Prove Arm/Orin can select a UART backend from platform/app configuration.
3. Prove Arm UART backend metadata can be supplied from device-tree or
   passthrough-style platform descriptions, including MMIO and IRQ ownership.
4. Identify which UART backends are shared and which are platform-specific:
   SBSA UART, PL011, 8250-compatible UART, Tegra HSUART, NVIDIA TCU, and QEMU
   chardev are not the same thing even when they feed the same mux core.
5. Prove x86 uses a QEMU serial/chardev backend later as backlog/regression
   work, not as a prerequisite for Orin completion.

### Phase 7: External-VM/QEMU Compatibility Proof

1. Confirm the same guest UART model contract can be backed by QEMU when the VM
   is run outside the seL4 VMM path.
2. Confirm `qemu_arm64` can reuse the Orin-first mux/demux shape.
3. Return to `qemu_x86_64` only after the Orin path is complete or when a
   specific regression/customer need requires it.

### Phase 8: Kconfig Off-Mode Proof

1. Build with the new mux Kconfig item enabled by default and confirm the
   generated mux path is active.
2. Build with the Kconfig item disabled from app-specific settings and confirm
   the app uses upstream-style console behavior.
3. Confirm host tooling and Autopilot do not expect mux introspection when the
   target was intentionally built in off mode.

## Risks And Open Questions

- The current x86 work tree contains uncommitted and untracked console routing
  docs/tools. Preserve those changes; do not "clean up" this topic by deleting
  them as unrelated. x86 mux completion is backlog unless the Orin path needs a
  specific shared fix from that code.
- The PL011 emulator cannot currently receive guest input. A muxed PL011 output
  proof is not enough; the target is a bidirectional Linux console device model
  with TX, RX, status, and interrupt behavior.
- Guest-facing UART buffering needs a timer owner. If the delayed flush is
  bolted onto the mux core, the mux will learn guest-device policy and become
  harder to reuse across hardware UART backends. Keep the timeout and line
  buffering at the UART sink/device-model boundary.
- A `50 ms` delayed flush is only the initial default. It must be configurable,
  and validation should check both throughput-oriented bursts and interactive
  prompts so the timeout does not hide shell responsiveness problems.
- Orin TCU depends on HSP/BPMP behavior. Historical TCU hangs mean the plan
  should avoid making TCU the only visibility path for early VM1 progress.
- Hardware UART backend scope must be kept separate from the mux core. Arm
  support must include device-tree/passthrough-style UART ownership metadata;
  otherwise the design will only work for virtual PL011-like paths and will
  fail the real-hardware requirement.
- Generated stream metadata is the first implementation requirement. Until it
  exists, the duplicated IDs in x86 CAmkES and host Python are prohibited
  migration targets, not acceptable interim architecture.
- Any implementation that accidentally uses `0xff` for our nested mux will
  collide with NVIDIA `vcmuxer` semantics and must be rejected.
- Physical UART bootstrap discovery on Orin still needs a concrete owner. The
  plan should move `/dev/ttyACM*` knowledge out of Autopilot chains, but some
  platform inventory or daemon layer must still identify the physical serial
  endpoint before mux introspection is available.
- "Every component has a stream" may produce more PTYs/logs than a human wants
  by default. That is a presentation policy issue; it must not collapse runtime
  identity back into shared default streams.
- The exact Kconfig symbol name and app-setting override shape are still open,
  but the behavior is fixed: default on, app-configurable, and upstream-style
  off mode.
