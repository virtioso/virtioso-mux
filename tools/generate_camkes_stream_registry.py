#!/usr/bin/env python3
#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

"""Generate CAmkES component identity metadata for mux introspection."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


COMPONENT_RE = re.compile(r"\bcomponent\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;")
COMPONENT_DEF_RE = re.compile(r"\bcomponent\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")
CAMKES_IMPORT_RE = re.compile(r"\bimport\s+<([^>]+)>;")
CONSOLE_INTERFACE_RE = re.compile(
    r"\b(?:maybe\s+)?(?:uses|provides)\s+(PutChar|GetChar)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;"
)
RESERVED_STREAM_IDS = {0, 0xFD, 0xFE}


def _sanitize_macro(name):
    token = re.sub(r"[^A-Za-z0-9_]", "_", name)
    token = re.sub(r"_+", "_", token).strip("_")
    if not token:
        token = "STREAM"
    if token[0].isdigit():
        token = "_" + token
    return token.upper()


def _json_escape_for_c(value):
    return json.dumps(value)


def _run_cpp(args):
    cmd = [args.cpp, "-P"]
    for include in args.cpp_include:
        cmd.append("-I" + str(include))
    cmd.extend(args.cpp_flag)
    cmd.append(str(args.camkes))
    try:
        return subprocess.run(
            cmd,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout
    except subprocess.CalledProcessError as err:
        if err.stderr:
            sys.stderr.write(err.stderr)
        raise SystemExit(err.returncode)


def _run_cpp_path(args, path):
    cmd = [args.cpp, "-P"]
    for include in args.cpp_include:
        cmd.append("-I" + str(include))
    cmd.extend(args.cpp_flag)
    cmd.append(str(path))
    try:
        return subprocess.run(
            cmd,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout
    except subprocess.CalledProcessError:
        return ""


def _extract_block(text, keyword):
    match = re.search(r"\b" + re.escape(keyword) + r"\s*\{", text)
    if not match:
        raise ValueError(f"missing '{keyword} {{' block")

    start = match.end()
    depth = 1
    pos = start
    while pos < len(text):
        char = text[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos]
        pos += 1

    raise ValueError(f"unterminated '{keyword}' block")


def _extract_braced_block(text, open_brace_pos):
    depth = 1
    pos = open_brace_pos + 1
    while pos < len(text):
        char = text[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_pos + 1:pos], pos + 1
        pos += 1
    raise ValueError("unterminated braced block")


def _iter_component_definitions(text):
    pos = 0
    while True:
        match = COMPONENT_DEF_RE.search(text, pos)
        if not match:
            break
        body, pos = _extract_braced_block(text, match.end() - 1)
        yield match.group(1), body


def _composition_text(preprocessed):
    assembly = _extract_block(preprocessed, "assembly")
    return _extract_block(assembly, "composition")


def _resolve_import(path, include_dirs):
    if path.is_absolute() and path.exists():
        return path
    matches = []
    for include_dir in include_dirs:
        candidate = include_dir / path
        if candidate.exists():
            return candidate
        candidate = include_dir / path.name
        if candidate.exists():
            return candidate

        matches.extend(
            candidate for candidate in include_dir.rglob(path.name) if candidate.is_file()
        )
    unique = sorted({candidate.resolve() for candidate in matches})
    if len(unique) == 1:
        return unique[0]
    return None


def _collect_preprocessed_camkes(args, root_preprocessed):
    texts = [root_preprocessed]
    seen = {args.camkes.resolve()}
    queue = [root_preprocessed]
    include_dirs = [Path(p) for p in args.cpp_include]
    include_dirs.append(args.camkes.parent)

    while queue:
        text = queue.pop(0)
        for import_name in CAMKES_IMPORT_RE.findall(text):
            import_path = Path(import_name)
            if import_path.suffix != ".camkes":
                continue
            resolved = _resolve_import(import_path, include_dirs)
            if not resolved:
                continue
            resolved = resolved.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            imported_text = _run_cpp_path(args, resolved)
            if not imported_text:
                continue
            texts.append(imported_text)
            queue.append(imported_text)
    return texts


def _component_type_metadata(preprocessed_texts):
    types = {}
    for text in preprocessed_texts:
        for component_type, body in _iter_component_definitions(text):
            interfaces = [
                {
                    "kind": match.group(1),
                    "name": match.group(2),
                }
                for match in CONSOLE_INTERFACE_RE.finditer(body)
            ]
            types[component_type] = {
                "console_interfaces": interfaces,
                "has_console_stream": bool(interfaces),
            }
    return types


def _next_stream_id(current):
    while current in RESERVED_STREAM_IDS:
        current += 1
    return current


def _stream_direction(interfaces):
    kinds = {interface["kind"] for interface in interfaces}
    if "PutChar" in kinds and "GetChar" in kinds:
        return "bidirectional"
    if "PutChar" in kinds:
        return "output"
    if "GetChar" in kinds:
        return "input"
    return "none"


def _component_instances(preprocessed, component_types):
    composition = _composition_text(preprocessed)
    streams = []
    seen = set()
    next_stream_id = 1
    for match in COMPONENT_RE.finditer(composition):
        component_type, component_name = match.groups()
        if component_name in seen:
            raise ValueError(f"duplicate component instance '{component_name}'")
        seen.add(component_name)
        type_meta = component_types.get(component_type, {})
        interfaces = type_meta.get("console_interfaces", [])
        stream_id = -1
        stream_id_source = "none"
        if type_meta.get("has_console_stream"):
            stream_id = _next_stream_id(next_stream_id)
            next_stream_id = stream_id + 1
            stream_id_source = "component.common.c"
        streams.append(
            {
                "stream_id": stream_id,
                "component": component_name,
                "type": component_type,
                "stream_id_source": stream_id_source,
                "direction": _stream_direction(interfaces),
                "interfaces": interfaces,
                "aliases": [],
            }
        )
    if not streams:
        raise ValueError("no component instances found in assembly composition")
    return streams


def _write_json(path, registry):
    path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")


def _write_header(path, registry):
    guard = _sanitize_macro(path.name) + "_"
    lines = [
        "/* Generated by generate_camkes_stream_registry.py. Do not edit. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "#define VIRTIO_CAMKES_STREAM_INVALID (-1)",
    ]
    for stream in registry["streams"]:
        lines.append(
            "#define VIRTIO_CAMKES_STREAM_%s (%d)"
            % (_sanitize_macro(stream["component"]), stream["stream_id"])
        )
    lines.extend(
        [
            "",
            "struct virtioso_camkes_stream_descriptor {",
            "    int32_t stream_id;",
            "    const char *component;",
            "    const char *type;",
            "    const char *stream_id_source;",
            "    const char *direction;",
            "};",
            "",
            "extern const struct virtioso_camkes_stream_descriptor",
            "    virtioso_camkes_stream_registry[];",
            "extern const size_t virtioso_camkes_stream_registry_count;",
            "extern const char virtioso_camkes_stream_registry_json[];",
            "",
            f"#endif /* {guard} */",
            "",
        ]
    )
    path.write_text("\n".join(lines))


def _write_c(path, header_name, registry):
    lines = [
        "/* Generated by generate_camkes_stream_registry.py. Do not edit. */",
        f'#include "{header_name}"',
        "",
        "const struct virtioso_camkes_stream_descriptor",
        "virtioso_camkes_stream_registry[] = {",
    ]
    for stream in registry["streams"]:
        lines.append(
            '    {%d, "%s", "%s", "%s", "%s"},'
            % (
                stream["stream_id"],
                stream["component"],
                stream["type"],
                stream["stream_id_source"],
                stream["direction"],
            )
        )
    lines.extend(
        [
            "};",
            "",
            "const size_t virtioso_camkes_stream_registry_count =",
            "    sizeof(virtioso_camkes_stream_registry) /",
            "    sizeof(virtioso_camkes_stream_registry[0]);",
            "",
            "const char virtioso_camkes_stream_registry_json[] =",
        ]
    )
    registry_json = json.dumps(registry, sort_keys=True)
    lines.append("    %s;" % _json_escape_for_c(registry_json))
    lines.append("")
    path.write_text("\n".join(lines))


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--camkes", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--cpp", default="cpp")
    parser.add_argument("--cpp-include", action="append", default=[], type=Path)
    parser.add_argument("--cpp-flag", action="append", default=[])
    args = parser.parse_args(argv)

    preprocessed = _run_cpp(args)
    preprocessed_texts = _collect_preprocessed_camkes(args, preprocessed)
    component_types = _component_type_metadata(preprocessed_texts)
    streams = _component_instances(preprocessed, component_types)
    registry = {
        "schema": "virtioso.camkes_component_identity.v1",
        "name": args.name,
        "source": str(args.camkes),
        "streams": streams,
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    _write_json(args.out_dir / "console-stream-registry.json", registry)
    _write_header(args.out_dir / "console_stream_ids.h", registry)
    _write_c(args.out_dir / "console_stream_registry.c", "console_stream_ids.h", registry)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
