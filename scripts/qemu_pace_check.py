#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
"""qemu_pace_check.py — measure apps/pace's frame rate from the host.

Starts the QEMU command given after `--` and timestamps every
"PACE: <nnn>" line the guest writes to its serial port at the moment it
arrives. The guest's own clock is what is under test, so the host's
monotonic clock is the reference. The rate is taken over the whole run:

    fps = (frames - 1) / (t_last - t_first)

Usage:
    qemu_pace_check.py [--label NAME] [--frames 600] [--hz 60]
        [--tolerance-pct 1.0] [--timeout 40] -- QEMU_COMMAND...

QEMU must send the serial port to its stdout (-serial stdio).

Exit 0 if frame numbers 0..frames-1 each arrived once, in order, and the
rate is within the tolerance of --hz; exit 1 otherwise. The measured
numbers are printed either way.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import threading
import time
from typing import IO

# GRUB writes escape sequences and a carriage return before the first line.
FRAME_RE = re.compile(rb"PACE: (\d{3})\s*$")
HALT_MARKER = b"Hello World from JOE"


def read_frames(stream: IO[bytes]) -> list[tuple[int, float]]:
    """(frame number, arrival time) per PACE line, up to the halt line."""
    frames: list[tuple[int, float]] = []
    for raw in iter(stream.readline, b""):
        stamp = time.monotonic()
        match = FRAME_RE.search(raw)
        if match:
            frames.append((int(match.group(1)), stamp))
        elif HALT_MARKER in raw:
            break
    return frames


def run_qemu(
    cmd: list[str], timeout: float, errors: IO[bytes]
) -> list[tuple[int, float]]:
    """Run QEMU, collect the PACE lines, and always stop it."""
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=errors,
    )
    watchdog = threading.Timer(timeout, proc.kill)
    watchdog.start()
    try:
        if proc.stdout is None:
            raise RuntimeError("QEMU stdout is not a pipe")
        return read_frames(proc.stdout)
    finally:
        watchdog.cancel()
        proc.kill()
        proc.wait()


def sequence_error(numbers: list[int], expected: int) -> str | None:
    """Why the frame numbers are not exactly 0..expected-1, or None."""
    if numbers == list(range(expected)):
        return None
    if len(numbers) != expected:
        return f"got {len(numbers)} PACE lines, expected {expected}"
    first = next(i for i, n in enumerate(numbers) if n != i)
    return f"frame {first} arrived as {numbers[first]}"


def measure(stamps: list[float]) -> dict[str, float]:
    """Seconds spanned, mean rate and the shortest/longest frame gap."""
    span = stamps[-1] - stamps[0]
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    return {
        "seconds": span,
        "fps": (len(stamps) - 1) / span,
        "min_gap_ms": min(gaps) * 1000.0,
        "max_gap_ms": max(gaps) * 1000.0,
    }


def verdict(
    m: dict[str, float], hz: float, tol_pct: float
) -> tuple[bool, str]:
    """Pass flag and the one-line summary for a measurement."""
    off_pct = (m["fps"] - hz) / hz * 100.0
    text = (
        f"{m['fps']:.3f} fps ({off_pct:+.3f}%, limit +-{tol_pct}%) over "
        f"{m['seconds']:.3f} s, frame gaps "
        f"{m['min_gap_ms']:.2f}..{m['max_gap_ms']:.2f} ms"
    )
    return abs(off_pct) <= tol_pct, text


def parse_args(argv: list[str]) -> argparse.Namespace:
    if "--" not in argv:
        raise SystemExit("usage: qemu_pace_check.py [options] -- QEMU_CMD...")
    split = argv.index("--")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="pace")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--hz", type=float, default=60.0)
    parser.add_argument("--tolerance-pct", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=40.0)
    args = parser.parse_args(argv[:split])
    args.cmd = argv[split + 1:]
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    with tempfile.TemporaryFile() as errors:
        frames = run_qemu(args.cmd, args.timeout, errors)
        errors.seek(0)
        qemu_stderr = errors.read().decode("utf-8", "replace").strip()
    bad = sequence_error([n for n, _ in frames], args.frames)
    if bad is not None or len(frames) < 2:
        print(f"FAIL: {args.label}: {bad}")
        if qemu_stderr:
            print(f"qemu stderr: {qemu_stderr}")
        return 1
    ok, text = verdict(
        measure([t for _, t in frames]), args.hz, args.tolerance_pct
    )
    status = "PASS" if ok else "FAIL"
    print(f"{status}: {args.label}: {args.frames} frames, {text}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
