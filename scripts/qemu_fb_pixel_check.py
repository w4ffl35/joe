#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
"""qemu_fb_pixel_check.py — QMP screendump + known-pixel colour check.

Drives the QEMU monitor (QMP) to screendump the running kernel's
display to a PPM file, then checks that specific coordinates
render_frame is known to draw match their expected RGB colour.

Usage:
    qemu_fb_pixel_check.py --qmp-sock PATH --ppm-out PATH \
        --check X,Y,R,G,B [--check X,Y,R,G,B ...]

Exit 0 if every checked pixel matches; exit 1 and print each mismatch
otherwise.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO


@dataclass(frozen=True)
class PixelCheck:
    x: int
    y: int
    r: int
    g: int
    b: int

    @staticmethod
    def parse(spec: str) -> "PixelCheck":
        parts = [int(p) for p in spec.split(",")]
        if len(parts) != 5:
            raise ValueError(f"bad --check spec: {spec!r}")
        x, y, r, g, b = parts
        return PixelCheck(x, y, r, g, b)


def _read_json_line(reader: IO[bytes]) -> dict[str, object]:
    """Read one newline-terminated JSON object from a buffered reader."""
    line = reader.readline()
    if not line:
        raise ConnectionError("QMP socket closed unexpectedly")
    return json.loads(line.decode("ascii"))


def qmp_call(
    reader: IO[bytes],
    sock: socket.socket,
    payload: dict[str, object],
) -> dict[str, object]:
    """Send one QMP command and return its return/error reply.

    QEMU may interleave asynchronous {"event": ...} messages before a
    command's actual reply; those are read here and skipped.
    """
    sock.sendall(json.dumps(payload).encode("ascii") + b"\n")
    reply = _read_json_line(reader)
    while "event" in reply:
        reply = _read_json_line(reader)
    return reply


def take_screendump(qmp_sock: Path, ppm_out: Path) -> None:
    """Negotiate QMP capabilities and screendump the display to a PPM."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(str(qmp_sock))
        with sock.makefile("rb") as reader:
            _read_json_line(reader)  # greeting
            reply = qmp_call(reader, sock, {"execute": "qmp_capabilities"})
            if "error" in reply:
                raise RuntimeError(f"qmp_capabilities failed: {reply}")
            reply = qmp_call(reader, sock, {
                "execute": "screendump",
                "arguments": {"filename": str(ppm_out)},
            })
            if "error" in reply:
                raise RuntimeError(f"screendump failed: {reply}")


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Parse a binary PPM (P6); return (width, height, pixel bytes)."""
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError("screendump is not a binary PPM (P6)")
    _, _, rest = data.partition(b"\n")
    tokens: list[bytes] = []
    while len(tokens) < 3:
        line, _, rest = rest.partition(b"\n")
        if line.startswith(b"#"):
            continue
        tokens.extend(line.split())
    width, height, maxval = (int(t) for t in tokens[:3])
    if maxval != 255:
        raise ValueError(f"unexpected PPM maxval {maxval}")
    return width, height, rest


def pixel_at(
    width: int, pixels: bytes, x: int, y: int,
) -> tuple[int, int, int]:
    """Return the (r, g, b) triplet at (x, y) of a row-major RGB buffer."""
    offset = (y * width + x) * 3
    return pixels[offset], pixels[offset + 1], pixels[offset + 2]


def check_pixels(
    width: int,
    height: int,
    pixels: bytes,
    checks: list[PixelCheck],
) -> list[str]:
    """Return one failure message per mismatch (empty when all pass)."""
    failures = []
    for c in checks:
        if c.x >= width or c.y >= height:
            failures.append(
                f"({c.x},{c.y}) is outside the {width}x{height} dump"
            )
            continue
        got = pixel_at(width, pixels, c.x, c.y)
        want = (c.r, c.g, c.b)
        if got != want:
            failures.append(f"({c.x},{c.y}): got rgb{got}, want rgb{want}")
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp-sock", required=True, type=Path)
    parser.add_argument("--ppm-out", required=True, type=Path)
    parser.add_argument(
        "--check",
        dest="checks",
        action="append",
        required=True,
        help="x,y,r,g,b (repeatable)",
    )
    return parser.parse_args()


def wait_for_file(path: Path, attempts: int = 20, delay: float = 0.1) -> None:
    """Poll for a non-empty file; raise if it never appears."""
    for _ in range(attempts):
        if path.exists() and path.stat().st_size > 0:
            return
        time.sleep(delay)
    raise RuntimeError(
        f"{path} did not appear within {attempts * delay:.1f}s of the "
        "screendump command being accepted"
    )


def main() -> int:
    args = parse_args()
    checks = [PixelCheck.parse(spec) for spec in args.checks]
    take_screendump(args.qmp_sock, args.ppm_out)
    wait_for_file(args.ppm_out)
    width, height, pixels = read_ppm(args.ppm_out)
    failures = check_pixels(width, height, pixels, checks)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    for c in checks:
        print(f"PASS: ({c.x},{c.y}) == rgb({c.r},{c.g},{c.b})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
