#!/usr/bin/env python3
"""Manifest-driven console router.

The initial live slice supports the current launcher-owned merged-console
topology by:

- validating a launcher-provided manifest
- preparing a stable runtime layout
- spawning a wrapped command
- mirroring command stdout/stderr to:
  - router stdout for compatibility
  - per-channel raw log
  - per-channel event log
  - an optional PTY for interactive attachment
- forwarding input from PTY and stdin back to the wrapped command
"""

from __future__ import annotations

import argparse
import base64
import errno
import json
import os
import pty
import selectors
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class ConsoleRouterError(RuntimeError):
    pass


@dataclass(frozen=True)
class Channel:
    id: int
    name: str
    kind: str
    interactive: bool
    pty: bool
    extras: dict[str, Any]


@dataclass(frozen=True)
class Manifest:
    version: int
    run_id: str
    target: str
    binary_name: str
    transport: dict[str, Any]
    channels: list[Channel]


class ChannelRuntime:
    def __init__(self, root: Path, channel: Channel):
        self.channel = channel
        self.root = root
        self.raw_log_path = root / "raw.log"
        self.events_log_path = root / "events.jsonl"
        self.pty_link_path = root / "pty"
        self._seq = 0
        self._pty_master_fd: int | None = None
        self._pty_slave_path: str | None = None

    def prepare(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        payload = {
            "id": self.channel.id,
            "name": self.channel.name,
            "kind": self.channel.kind,
            "interactive": self.channel.interactive,
            "pty": self.channel.pty,
            "raw_log": "raw.log",
            "events_log": "events.jsonl",
            **self.channel.extras,
        }
        (self.root / "channel.json").write_text(json.dumps(payload, indent=2) + "\n")
        self.raw_log_path.touch()
        self.events_log_path.touch()
        if self.channel.pty:
            master_fd, slave_fd = pty.openpty()
            self._pty_master_fd = master_fd
            self._pty_slave_path = os.ttyname(slave_fd)
            os.close(slave_fd)
            if self.pty_link_path.exists() or self.pty_link_path.is_symlink():
                self.pty_link_path.unlink()
            self.pty_link_path.symlink_to(self._pty_slave_path)

    @property
    def pty_path(self) -> str | None:
        return self._pty_slave_path

    @property
    def pty_master_fd(self) -> int | None:
        return self._pty_master_fd

    def close(self) -> None:
        if self._pty_master_fd is not None:
            os.close(self._pty_master_fd)
            self._pty_master_fd = None

    def emit_event(self, direction: str, data: bytes) -> None:
        self._seq += 1
        event = {
            "source": self.channel.name,
            "dir": direction,
            "seq": self._seq,
            "clock_monotonic_ns": time.monotonic_ns(),
            "clock_realtime_ns": time.time_ns(),
            "payload_b64": base64.b64encode(data).decode("ascii"),
            "payload_len": len(data),
        }
        with open(self.events_log_path, "a", encoding="utf-8", buffering=1) as f:
            f.write(json.dumps(event) + "\n")

    def append_raw(self, data: bytes) -> None:
        with open(self.raw_log_path, "ab", buffering=0) as f:
            f.write(data)

    def forward_to_pty(self, data: bytes) -> None:
        if self._pty_master_fd is None:
            return
        try:
            os.write(self._pty_master_fd, data)
        except OSError:
            return


_ANSI_ESCAPE_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def _channel_runtimes(runtime_dir: Path, manifest: Manifest) -> list[ChannelRuntime]:
    channels_root = runtime_dir / "channels"
    channels_root.mkdir(parents=True, exist_ok=True)
    return [ChannelRuntime(channels_root / channel.name, channel) for channel in manifest.channels]


def _prepare_channel_runtimes(runtime_dir: Path, manifest: Manifest) -> list[ChannelRuntime]:
    runtime_dir.mkdir(parents=True, exist_ok=True)
    (runtime_dir / "runtime-manifest.json").write_text(json.dumps(_runtime_manifest_payload(manifest), indent=2) + "\n")
    runtimes = _channel_runtimes(runtime_dir, manifest)
    for runtime in runtimes:
        runtime.prepare()
    _write_sessions_manifest(runtime_dir, runtimes)
    return runtimes


def _close_channel_runtimes(runtimes: list[ChannelRuntime]) -> None:
    for runtime in runtimes:
        runtime.close()


def _channel_runtime_map(runtimes: list[ChannelRuntime]) -> dict[str, ChannelRuntime]:
    return {runtime.channel.name: runtime for runtime in runtimes}


def _require_transport_str(transport: dict[str, Any], key: str) -> str:
    value = transport.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ConsoleRouterError(f"transport field '{key}' must be a non-empty string")
    return value


def _encode_jsonl_frame(channel_name: str, direction: str, data: bytes) -> bytes:
    payload = {
        "channel": channel_name,
        "dir": direction,
        "payload_b64": base64.b64encode(data).decode("ascii"),
    }
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


def _decode_jsonl_frame(line: bytes) -> tuple[str, str, bytes]:
    try:
        payload = json.loads(line.decode("utf-8"))
    except Exception as exc:
        raise ConsoleRouterError(f"invalid jsonl frame: {exc}") from exc
    if not isinstance(payload, dict):
        raise ConsoleRouterError("invalid jsonl frame: root must be object")
    channel_name = payload.get("channel")
    direction = payload.get("dir")
    payload_b64 = payload.get("payload_b64")
    if not isinstance(channel_name, str) or not channel_name:
        raise ConsoleRouterError("invalid jsonl frame: missing channel")
    if not isinstance(direction, str) or not direction:
        raise ConsoleRouterError("invalid jsonl frame: missing dir")
    if not isinstance(payload_b64, str):
        raise ConsoleRouterError("invalid jsonl frame: missing payload_b64")
    try:
        data = base64.b64decode(payload_b64, validate=True)
    except Exception as exc:
        raise ConsoleRouterError(f"invalid jsonl frame payload: {exc}") from exc
    return channel_name, direction, data


def _strip_ansi_bytes(data: bytes) -> bytes:
    return _ANSI_ESCAPE_RE.sub(b"", data)


def _decode_prefixed_line(
    line: bytes,
    prefix_map: dict[str, str],
    fallback_channel: str | None,
) -> tuple[str | None, bytes]:
    normalized = _strip_ansi_bytes(line)
    for prefix, channel_name in prefix_map.items():
        prefix_bytes = prefix.encode("utf-8")
        if normalized.startswith(prefix_bytes):
            payload = normalized[len(prefix_bytes):]
            return channel_name, payload
    return fallback_channel, normalized


def _write_proc_input(stdin_fd: int | None, data: bytes) -> None:
    if stdin_fd is None or not data:
        return
    try:
        os.write(stdin_fd, data)
    except OSError:
        return


def _register_input_sources(
    selector: selectors.BaseSelector,
    runtimes: list[ChannelRuntime],
) -> None:
    for runtime in runtimes:
        if runtime.pty_master_fd is None:
            continue
        _set_nonblocking(runtime.pty_master_fd)
        selector.register(runtime.pty_master_fd, selectors.EVENT_READ, ("pty_in", runtime.channel.name))
    try:
        selector.register(sys.stdin.fileno(), selectors.EVENT_READ, ("stdin_in", None))
    except Exception:
        pass


def _flush_line_prefix_buffer(
    buffer: bytearray,
    *,
    emit_partial: bool,
    prefix_map: dict[str, str],
    fallback_channel: str | None,
    runtime_by_name: dict[str, ChannelRuntime],
) -> None:
    while True:
        newline_idx = buffer.find(b"\n")
        if newline_idx < 0:
            break
        line = bytes(buffer[:newline_idx + 1])
        del buffer[:newline_idx + 1]
        channel_name, payload = _decode_prefixed_line(line, prefix_map, fallback_channel)
        if channel_name is None:
            continue
        runtime = runtime_by_name.get(channel_name)
        if runtime is None:
            raise ConsoleRouterError(f"line_prefixes referenced unknown channel {channel_name!r}")
        runtime.append_raw(payload)
        runtime.emit_event("rx", payload)
        runtime.forward_to_pty(payload)
    if emit_partial and buffer:
        partial = bytes(buffer)
        # Preserve prefixed VMM/control lines until newline so their classification
        # stays stable. Flush unprefixed fallback traffic incrementally so prompts
        # like "driver-vm login:" are visible without a trailing newline.
        if any(partial.startswith(prefix.encode("utf-8")) for prefix in prefix_map):
            return
        line = partial
        buffer.clear()
        channel_name, payload = _decode_prefixed_line(line, prefix_map, fallback_channel)
        if channel_name is None:
            return
        runtime = runtime_by_name.get(channel_name)
        if runtime is None:
            raise ConsoleRouterError(f"line_prefixes referenced unknown channel {channel_name!r}")
        runtime.append_raw(payload)
        runtime.emit_event("rx", payload)
        runtime.forward_to_pty(payload)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise ConsoleRouterError(f"manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ConsoleRouterError(f"invalid JSON in manifest {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ConsoleRouterError(f"manifest root must be an object: {path}")
    return data


def _require_str(data: dict[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ConsoleRouterError(f"manifest field '{key}' must be a non-empty string")
    return value


def _require_int(data: dict[str, Any], key: str) -> int:
    value = data.get(key)
    if not isinstance(value, int):
        raise ConsoleRouterError(f"manifest field '{key}' must be an integer")
    return value


def _require_bool(data: dict[str, Any], key: str) -> bool:
    value = data.get(key)
    if not isinstance(value, bool):
        raise ConsoleRouterError(f"manifest field '{key}' must be a boolean")
    return value


def load_manifest(path: Path) -> Manifest:
    data = _load_json(path)
    version = _require_int(data, "version")
    if version != 1:
        raise ConsoleRouterError(f"unsupported manifest version: {version}")

    transport = data.get("transport")
    if not isinstance(transport, dict):
        raise ConsoleRouterError("manifest field 'transport' must be an object")

    raw_channels = data.get("channels")
    if not isinstance(raw_channels, list) or not raw_channels:
        raise ConsoleRouterError("manifest field 'channels' must be a non-empty list")

    channels: list[Channel] = []
    seen_ids: set[int] = set()
    seen_names: set[str] = set()
    for idx, entry in enumerate(raw_channels):
        if not isinstance(entry, dict):
            raise ConsoleRouterError(f"channel[{idx}] must be an object")
        channel_id = _require_int(entry, "id")
        name = _require_str(entry, "name")
        kind = _require_str(entry, "kind")
        interactive = _require_bool(entry, "interactive")
        pty_enabled = _require_bool(entry, "pty")
        if channel_id in seen_ids:
            raise ConsoleRouterError(f"duplicate channel id: {channel_id}")
        if name in seen_names:
            raise ConsoleRouterError(f"duplicate channel name: {name}")
        seen_ids.add(channel_id)
        seen_names.add(name)
        extras = {
            k: v
            for k, v in entry.items()
            if k not in {"id", "name", "kind", "interactive", "pty"}
        }
        channels.append(Channel(channel_id, name, kind, interactive, pty_enabled, extras))

    return Manifest(
        version=version,
        run_id=_require_str(data, "run_id"),
        target=_require_str(data, "target"),
        binary_name=_require_str(data, "binary_name"),
        transport=transport,
        channels=channels,
    )


def _runtime_manifest_payload(manifest: Manifest) -> dict[str, Any]:
    return {
        "version": manifest.version,
        "run_id": manifest.run_id,
        "target": manifest.target,
        "binary_name": manifest.binary_name,
        "transport": manifest.transport,
        "channels": [
            {
                "id": channel.id,
                "name": channel.name,
                "kind": channel.kind,
                "interactive": channel.interactive,
                "pty": channel.pty,
                "runtime_dir": f"channels/{channel.name}",
                "raw_log": f"channels/{channel.name}/raw.log",
                "events_log": f"channels/{channel.name}/events.jsonl",
                "pty_link": f"channels/{channel.name}/pty" if channel.pty else None,
                **channel.extras,
            }
            for channel in manifest.channels
        ],
    }


def prepare_runtime(manifest_path: Path, runtime_dir: Path) -> int:
    manifest = load_manifest(manifest_path)
    runtimes: list[ChannelRuntime] = []
    try:
        runtimes = _prepare_channel_runtimes(runtime_dir, manifest)
    finally:
        _close_channel_runtimes(runtimes)

    print(runtime_dir, flush=True)
    return 0


def _write_sessions_manifest(runtime_dir: Path, runtimes: list[ChannelRuntime]) -> None:
    sessions = []
    for runtime in runtimes:
        sessions.append(
            {
                "session_id": runtime.channel.name,
                "name": runtime.channel.name,
                "kind": runtime.channel.kind,
                "interactive": runtime.channel.interactive,
                "log_path": str(runtime.raw_log_path),
                "events_path": str(runtime.events_log_path),
                "pty_path": runtime.pty_path,
            }
        )
    payload = {
        "version": 1,
        "sessions": sessions,
    }
    (runtime_dir / "sessions.json").write_text(json.dumps(payload, indent=2) + "\n")


def describe_manifest(manifest_path: Path) -> int:
    manifest = load_manifest(manifest_path)
    payload = _runtime_manifest_payload(manifest)
    print(json.dumps(payload, indent=2), flush=True)
    return 0


def _set_nonblocking(fd: int) -> None:
    os.set_blocking(fd, False)


def run_command(manifest_path: Path, runtime_dir: Path, command: list[str]) -> int:
    manifest = load_manifest(manifest_path)
    transport_type = manifest.transport.get("type")
    if transport_type not in {"process_stdio", "jsonl_frames", "line_prefixes"}:
        raise ConsoleRouterError(
            "run-command currently supports only transport.type=process_stdio, jsonl_frames, or line_prefixes, "
            f"got {transport_type!r}"
        )
    if transport_type == "process_stdio" and len(manifest.channels) != 1:
        raise ConsoleRouterError("run-command with process_stdio expects exactly one channel in the manifest")

    runtimes: list[ChannelRuntime] = []
    runtime_by_name: dict[str, ChannelRuntime] = {}
    primary_runtime: ChannelRuntime | None = None
    default_input_channel: str | None = None
    framed_out_buffer = bytearray()
    line_prefix_buffer = bytearray()
    line_prefix_map: dict[str, str] = {}
    fallback_channel: str | None = None

    runtimes = _prepare_channel_runtimes(runtime_dir, manifest)
    runtime_by_name = _channel_runtime_map(runtimes)
    if transport_type == "process_stdio":
        primary_runtime = runtimes[0]
    elif transport_type == "jsonl_frames":
        default_input_channel = _require_transport_str(manifest.transport, "default_input_channel")
        if default_input_channel not in runtime_by_name:
            raise ConsoleRouterError(
                f"transport default_input_channel {default_input_channel!r} does not match any manifest channel"
            )
    else:
        default_input_channel = _require_transport_str(manifest.transport, "default_input_channel")
        if default_input_channel not in runtime_by_name:
            raise ConsoleRouterError(
                f"transport default_input_channel {default_input_channel!r} does not match any manifest channel"
            )
        prefix_map_raw = manifest.transport.get("prefix_map")
        if not isinstance(prefix_map_raw, dict) or not prefix_map_raw:
            raise ConsoleRouterError("line_prefixes transport requires a non-empty transport.prefix_map object")
        for prefix, channel_name in prefix_map_raw.items():
            if not isinstance(prefix, str) or not prefix:
                raise ConsoleRouterError("line_prefixes prefix_map keys must be non-empty strings")
            if not isinstance(channel_name, str) or not channel_name:
                raise ConsoleRouterError("line_prefixes prefix_map values must be non-empty strings")
            if channel_name not in runtime_by_name:
                raise ConsoleRouterError(
                    f"line_prefixes prefix_map references unknown channel {channel_name!r}"
                )
            line_prefix_map[prefix] = channel_name
        fallback = manifest.transport.get("fallback_channel")
        if fallback is not None:
            if not isinstance(fallback, str) or not fallback:
                raise ConsoleRouterError("line_prefixes fallback_channel must be a non-empty string when present")
            if fallback not in runtime_by_name:
                raise ConsoleRouterError(
                    f"line_prefixes fallback_channel references unknown channel {fallback!r}"
                )
            fallback_channel = fallback

    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
    )
    if proc.stdout is None:
        raise ConsoleRouterError("wrapped command stdout unavailable")

    stdout_fd = proc.stdout.fileno()
    stdin_fd = proc.stdin.fileno() if proc.stdin is not None else None
    _set_nonblocking(stdout_fd)
    if stdin_fd is not None:
        _set_nonblocking(stdin_fd)
    try:
        _set_nonblocking(sys.stdin.fileno())
    except OSError:
        pass

    selector = selectors.DefaultSelector()
    selector.register(stdout_fd, selectors.EVENT_READ, ("proc_out", None))
    _register_input_sources(selector, runtimes)

    return_code = 0
    try:
        while True:
            events = selector.select(timeout=0.1)
            for key, _mask in events:
                kind, payload = key.data
                if kind == "proc_out":
                    try:
                        data = os.read(stdout_fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            data = b""
                        else:
                            raise
                    if data:
                        if transport_type == "process_stdio":
                            assert primary_runtime is not None
                            os.write(sys.stdout.fileno(), data)
                            primary_runtime.append_raw(data)
                            primary_runtime.emit_event("rx", data)
                            primary_runtime.forward_to_pty(data)
                        elif transport_type == "jsonl_frames":
                            framed_out_buffer.extend(data)
                            while True:
                                newline_idx = framed_out_buffer.find(b"\n")
                                if newline_idx < 0:
                                    break
                                line = bytes(framed_out_buffer[:newline_idx])
                                del framed_out_buffer[:newline_idx + 1]
                                if not line:
                                    continue
                                channel_name, direction, frame_data = _decode_jsonl_frame(line)
                                runtime = runtime_by_name.get(channel_name)
                                if runtime is None:
                                    raise ConsoleRouterError(
                                        f"jsonl frame referenced unknown channel {channel_name!r}"
                                    )
                                runtime.append_raw(frame_data)
                                runtime.emit_event(direction, frame_data)
                                if direction == "rx":
                                    runtime.forward_to_pty(frame_data)
                        else:
                            os.write(sys.stdout.fileno(), data)
                            line_prefix_buffer.extend(data)
                            _flush_line_prefix_buffer(
                                line_prefix_buffer,
                                emit_partial=False,
                                prefix_map=line_prefix_map,
                                fallback_channel=fallback_channel,
                                runtime_by_name=runtime_by_name,
                            )
                    else:
                        selector.unregister(stdout_fd)
                elif kind in {"pty_in", "stdin_in"}:
                    read_fd = key.fd
                    try:
                        data = os.read(read_fd, 4096)
                    except OSError as exc:
                        if exc.errno in {errno.EIO, errno.EBADF}:
                            data = b""
                        else:
                            raise
                    if data:
                        if transport_type == "process_stdio":
                            assert primary_runtime is not None
                            primary_runtime.emit_event("tx", data)
                            _write_proc_input(stdin_fd, data)
                        elif transport_type == "jsonl_frames":
                            input_channel = payload or default_input_channel
                            runtime = runtime_by_name.get(str(input_channel)) if input_channel is not None else None
                            if runtime is None:
                                raise ConsoleRouterError(
                                    f"input referenced unknown channel {input_channel!r}"
                                )
                            runtime.emit_event("tx", data)
                            _write_proc_input(stdin_fd, _encode_jsonl_frame(runtime.channel.name, "tx", data))
                        else:
                            input_channel = payload or default_input_channel
                            runtime = runtime_by_name.get(str(input_channel)) if input_channel is not None else None
                            if runtime is None:
                                raise ConsoleRouterError(
                                    f"input referenced unknown channel {input_channel!r}"
                                )
                            runtime.emit_event("tx", data)
                            _write_proc_input(stdin_fd, data)
                    elif not data:
                        try:
                            selector.unregister(read_fd)
                        except Exception:
                            pass

            polled = proc.poll()
            if polled is not None:
                return_code = polled
                try:
                    trailing = os.read(stdout_fd, 4096)
                except OSError:
                    trailing = b""
                if trailing:
                    if transport_type == "process_stdio":
                        assert primary_runtime is not None
                        os.write(sys.stdout.fileno(), trailing)
                        primary_runtime.append_raw(trailing)
                        primary_runtime.emit_event("rx", trailing)
                        primary_runtime.forward_to_pty(trailing)
                    elif transport_type == "jsonl_frames":
                        framed_out_buffer.extend(trailing)
                    else:
                        os.write(sys.stdout.fileno(), trailing)
                        line_prefix_buffer.extend(trailing)
                if transport_type == "jsonl_frames" and framed_out_buffer.strip():
                    raise ConsoleRouterError(
                        "jsonl_frames transport ended with an incomplete frame in the output buffer"
                    )
                if transport_type == "line_prefixes":
                    _flush_line_prefix_buffer(
                        line_prefix_buffer,
                        emit_partial=True,
                        prefix_map=line_prefix_map,
                        fallback_channel=fallback_channel,
                        runtime_by_name=runtime_by_name,
                    )
                break
    finally:
        selector.close()
        _close_channel_runtimes(runtimes)
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)

    return return_code


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    describe = sub.add_parser("describe-manifest", help="Validate and print normalized manifest data")
    describe.add_argument("--manifest", required=True, help="Path to console-manifest.json")
    describe.set_defaults(handler="describe_manifest")

    prepare = sub.add_parser("prepare-runtime", help="Validate manifest and create channel runtime layout")
    prepare.add_argument("--manifest", required=True, help="Path to console-manifest.json")
    prepare.add_argument("--runtime-dir", required=True, help="Directory for channel runtime state")
    prepare.set_defaults(handler="prepare_runtime")

    run = sub.add_parser("run-command", help="Run a command through the current manifest-driven router")
    run.add_argument("--manifest", required=True, help="Path to console-manifest.json")
    run.add_argument("--runtime-dir", required=True, help="Directory for router runtime state")
    run.add_argument("cmd", nargs=argparse.REMAINDER, help="Command to run after '--'")
    run.set_defaults(handler="run_command")

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.handler == "describe_manifest":
            return describe_manifest(Path(args.manifest).expanduser().resolve())
        if args.handler == "prepare_runtime":
            return prepare_runtime(
                Path(args.manifest).expanduser().resolve(),
                Path(args.runtime_dir).expanduser().resolve(),
            )
        if args.handler == "run_command":
            command = list(args.cmd)
            if command and command[0] == "--":
                command = command[1:]
            if not command:
                raise ConsoleRouterError("run-command requires a non-empty command after '--'")
            return run_command(
                Path(args.manifest).expanduser().resolve(),
                Path(args.runtime_dir).expanduser().resolve(),
                command,
            )
        raise ConsoleRouterError(f"unhandled command: {args.handler}")
    except ConsoleRouterError as exc:
        print(f"CONSOLE_ROUTER_ERROR: {exc}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
