#!/usr/bin/env python3
"""Manifest-driven console router runtime preparer.

This tool is the backend-owned seam for console routing metadata. The initial
slice validates a launcher-provided manifest and prepares a stable runtime
layout for channels. Later slices can extend this same tool into a live router
that owns PTYs, byte forwarding, and event capture.
"""

from __future__ import annotations

import argparse
import json
import sys
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
        pty = _require_bool(entry, "pty")
        if channel_id in seen_ids:
            raise ConsoleRouterError(f"duplicate channel id: {channel_id}")
        if name in seen_names:
            raise ConsoleRouterError(f"duplicate channel name: {name}")
        seen_ids.add(channel_id)
        seen_names.add(name)
        extras = {k: v for k, v in entry.items() if k not in {"id", "name", "kind", "interactive", "pty"}}
        channels.append(Channel(channel_id, name, kind, interactive, pty, extras))

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
                **channel.extras,
            }
            for channel in manifest.channels
        ],
    }


def prepare_runtime(manifest_path: Path, runtime_dir: Path) -> int:
    manifest = load_manifest(manifest_path)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    channels_root = runtime_dir / "channels"
    channels_root.mkdir(parents=True, exist_ok=True)

    runtime_manifest = _runtime_manifest_payload(manifest)
    (runtime_dir / "runtime-manifest.json").write_text(json.dumps(runtime_manifest, indent=2) + "\n")

    for channel in manifest.channels:
        channel_dir = channels_root / channel.name
        channel_dir.mkdir(parents=True, exist_ok=True)
        channel_payload = {
            "id": channel.id,
            "name": channel.name,
            "kind": channel.kind,
            "interactive": channel.interactive,
            "pty": channel.pty,
            "raw_log": "raw.log",
            "events_log": "events.jsonl",
            **channel.extras,
        }
        (channel_dir / "channel.json").write_text(json.dumps(channel_payload, indent=2) + "\n")
        (channel_dir / "raw.log").touch()
        (channel_dir / "events.jsonl").touch()

    print(runtime_dir, flush=True)
    return 0


def describe_manifest(manifest_path: Path) -> int:
    manifest = load_manifest(manifest_path)
    payload = _runtime_manifest_payload(manifest)
    print(json.dumps(payload, indent=2), flush=True)
    return 0


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
        raise ConsoleRouterError(f"unhandled command: {args.handler}")
    except ConsoleRouterError as exc:
        print(f"CONSOLE_ROUTER_ERROR: {exc}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
