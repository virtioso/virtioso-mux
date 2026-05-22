#!/usr/bin/env python3
"""Bridge a dedicated QEMU mux uplink socket to stdin/stdout.

This helper launches a QEMU-producing command, captures its legacy stdout/stderr
to a side log, and exposes only the dedicated unix-socket uplink on its own
stdin/stdout. It is intended for the early "second QEMU UART" migration slice,
where authoritative framed console traffic should stop sharing the primary
legacy serial/stdout lane.
"""

from __future__ import annotations

import argparse
import errno
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import sys
import time


class BridgeError(RuntimeError):
    pass


def _set_nonblocking(fd: int) -> None:
    os.set_blocking(fd, False)


def _wait_for_socket_connect(socket_path: Path, child: subprocess.Popen[bytes], timeout_s: float) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if child.poll() is not None and not socket_path.exists():
            raise BridgeError(
                f"producer exited before dedicated mux uplink appeared: rc={child.returncode}"
            )
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(str(socket_path))
            sock.setblocking(False)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise BridgeError(f"unable to connect dedicated mux uplink {socket_path}: {last_error}")


def run_bridge(socket_path: Path, legacy_log: Path, command: list[str], connect_timeout_s: float) -> int:
    if socket_path.exists():
        socket_path.unlink()
    legacy_log.parent.mkdir(parents=True, exist_ok=True)
    with open(legacy_log, "ab", buffering=0) as legacy_file:
        child = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=legacy_file,
            stderr=subprocess.STDOUT,
            bufsize=0,
            start_new_session=True,
        )
        sock: socket.socket | None = None
        selector = selectors.DefaultSelector()
        try:
            sock = _wait_for_socket_connect(socket_path, child, connect_timeout_s)
            selector.register(sock, selectors.EVENT_READ, "sock")
            try:
                _set_nonblocking(sys.stdin.fileno())
                selector.register(sys.stdin.fileno(), selectors.EVENT_READ, "stdin")
            except OSError:
                pass

            while True:
                events = selector.select(timeout=0.1)
                for key, _mask in events:
                    if key.data == "sock":
                        try:
                            data = sock.recv(4096)
                        except OSError as exc:
                            if exc.errno == errno.EAGAIN:
                                continue
                            else:
                                raise
                        if data:
                            os.write(sys.stdout.fileno(), data)
                        else:
                            selector.unregister(sock)
                            sock.close()
                            sock = None
                    elif key.data == "stdin":
                        try:
                            data = os.read(key.fd, 4096)
                        except OSError as exc:
                            if exc.errno in {errno.EIO, errno.EBADF}:
                                data = b""
                            else:
                                raise
                        if data and sock is not None:
                            sock.sendall(data)
                        elif not data:
                            try:
                                selector.unregister(key.fd)
                            except Exception:
                                pass

                rc = child.poll()
                if rc is not None and sock is None:
                    return rc
        finally:
            selector.close()
            if sock is not None:
                sock.close()
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                    child.wait(timeout=3.0)
                except Exception:
                    try:
                        os.killpg(child.pid, signal.SIGKILL)
                    except Exception:
                        pass
            if socket_path.exists():
                try:
                    socket_path.unlink()
                except OSError:
                    pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket-path", required=True, help="Unix socket path exposed by QEMU for the mux uplink")
    parser.add_argument("--legacy-log", required=True, help="Path for legacy producer stdout/stderr capture")
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=15.0,
        help="Seconds to wait for the QEMU mux uplink socket",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command or args.command[0] != "--" or len(args.command) < 2:
        raise BridgeError("command requires a non-empty argv after '--'")
    return run_bridge(
        Path(args.socket_path),
        Path(args.legacy_log),
        args.command[1:],
        args.connect_timeout,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BridgeError as exc:
        print(f"QEMU_MUX_UPLINK_BRIDGE_ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
