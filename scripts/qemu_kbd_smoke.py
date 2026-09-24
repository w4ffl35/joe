#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
"""qemu_kbd_smoke.py — the polled PS/2 keyboard gate.

Boots apps/keys (bundled by `make qemu-kbd-smoke`) under QEMU, sends key
events over QMP and checks the serial lines the app prints for each step
(scripts/qemu_kbd_steps.py lists them and says what the lines mean). Events
go out with QMP `input-send-event`, where one call is one batch that is
queued in the controller before the guest can read any of it, or, once,
with `send-key` and a hold time.

Usage: qemu_kbd_smoke.py [--kernel ELF] [--iso ISO] [--accel tcg|kvm ...]
           --out-dir DIR
Each image given is booted under each accelerator (tcg by default) and run
through every step; exit 0 if all pass.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import TracebackType
from typing import IO

from qemu_fb_pixel_check import qmp_call
from qemu_kbd_steps import Step, all_steps

READY = "KEYS: ready"
READY_S = 20.0
WAIT_S = 5.0
SETTLE_S = 0.25
POLL_S = 0.02


def qcode(key: str) -> dict[str, object]:
    return {"type": "qcode", "data": key}


def key_event(key: str, is_down: bool) -> dict[str, object]:
    return {"type": "key", "data": {"down": is_down, "key": qcode(key)}}


class Guest:
    """One QEMU boot: its serial log and its QMP connection."""

    def __init__(self, label: str, boot: list[str], out_dir: Path) -> None:
        out_dir.mkdir(parents=True, exist_ok=True)
        self.serial_path = out_dir / f"{label}-serial.log"
        self.sock_path = out_dir / f"{label}.sock"
        for stale in (self.serial_path, self.sock_path):
            stale.unlink(missing_ok=True)
        self.command = [
            "qemu-system-x86_64", "-display", "none", "-monitor", "none",
            "-no-reboot", "-net", "none",
            "-serial", f"file:{self.serial_path}",
            "-qmp", f"unix:{self.sock_path},server=on,wait=off", *boot]
        self.mark = 0
        self.process: subprocess.Popen[bytes] | None = None
        self.sock: socket.socket | None = None
        self.reader: IO[bytes] | None = None

    def __enter__(self) -> Guest:
        self.process = subprocess.Popen(
            self.command, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connect()
        return self

    def __exit__(self, kind: type[BaseException] | None,
                 error: BaseException | None,
                 trace: TracebackType | None) -> None:
        if self.process is not None:
            self.process.kill()
            self.process.wait()
        if self.sock is not None:
            self.sock.close()

    def connect(self) -> None:
        """Waits for the QMP socket, reads the greeting and negotiates."""
        assert self.sock is not None
        deadline = time.monotonic() + READY_S
        while not self.sock_path.exists():
            if time.monotonic() > deadline:
                raise RuntimeError("QEMU never opened its QMP socket")
            time.sleep(POLL_S)
        self.sock.connect(str(self.sock_path))
        self.reader = self.sock.makefile("rb")
        self.reader.readline()
        self.call({"execute": "qmp_capabilities"})

    def call(self, payload: dict[str, object]) -> dict[str, object]:
        assert self.sock is not None and self.reader is not None
        reply = qmp_call(self.reader, self.sock, payload)
        if "error" in reply:
            raise RuntimeError(f"QMP {payload['execute']} failed: {reply}")
        return reply

    def send(self, step: Step) -> None:
        if step.hold_ms is None:
            events = [key_event(key, is_down) for key, is_down in step.events]
            self.call({"execute": "input-send-event",
                       "arguments": {"events": events}})
            return
        self.call({"execute": "send-key", "arguments": {
            "keys": [qcode(step.events[0][0])],
            "hold-time": step.hold_ms}})

    def serial(self) -> str:
        if not self.serial_path.exists():
            return ""
        return self.serial_path.read_text(errors="replace")

    def new_lines(self) -> tuple[list[str], int]:
        """The complete serial lines after the mark, and the offset that
        follows the last of them."""
        text = self.serial()
        end = text.rfind("\n") + 1
        if end <= self.mark:
            return [], self.mark
        return text[self.mark:end].splitlines(), end

    def wait_ready(self) -> None:
        deadline = time.monotonic() + READY_S
        while READY not in self.serial():
            if time.monotonic() > deadline:
                raise RuntimeError(f"no {READY!r} on serial")
            time.sleep(POLL_S)
        self.mark = self.serial().index(READY) + len(READY) + 1


def run_step(guest: Guest, step: Step) -> str | None:
    """Sends the step and returns what is wrong with the serial lines that
    follow it, or None. Waits for the lines expected, then for a moment
    longer, so that a line that should not be there is seen."""
    guest.send(step)
    deadline = time.monotonic() + WAIT_S
    lines, _ = guest.new_lines()
    while len(lines) < len(step.expect) and time.monotonic() < deadline:
        time.sleep(POLL_S)
        lines, _ = guest.new_lines()
    time.sleep(SETTLE_S)
    lines, guest.mark = guest.new_lines()
    if lines == step.expect:
        return None
    return f"expected {step.expect}, serial lines were {lines}"


def run_boot(label: str, boot: list[str], out_dir: Path) -> int:
    """Boots one image and runs every step; returns the failure count."""
    print(f"kbd-smoke: {label}")
    try:
        with Guest(label, boot, out_dir) as guest:
            guest.wait_ready()
            for step in all_steps():
                problem = run_step(guest, step)
                print(f"  {'FAIL' if problem else 'PASS'}: {step.name}")
                if problem:
                    print(f"    {problem}", file=sys.stderr)
                    return 1
    except (RuntimeError, OSError) as error:
        print(f"  FAIL: {error}", file=sys.stderr)
        return 1
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--accel", action="append", choices=["tcg", "kvm"])
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()
    if args.kernel is None and args.iso is None:
        parser.error("give --kernel, --iso or both")
    args.accel = args.accel or ["tcg"]
    return args


def boot_options(args: argparse.Namespace) -> list[tuple[str, list[str]]]:
    """A label and QEMU arguments for each image under each accelerator."""
    images: list[tuple[str, list[str]]] = []
    if args.kernel is not None:
        images.append(("pvh", ["-kernel", str(args.kernel)]))
    if args.iso is not None:
        images.append(("iso", ["-cdrom", str(args.iso), "-boot", "d"]))
    return [(f"{name}-{accel}", ["-accel", accel, *boot])
            for accel in args.accel for name, boot in images]


def main() -> int:
    args = parse_args()
    boots = boot_options(args)
    failures = sum(run_boot(label, boot, args.out_dir)
                   for label, boot in boots)
    if failures:
        print(f"FAIL: kbd-smoke — {failures} boot(s) failed", file=sys.stderr)
        return 1
    print(f"PASS: kbd-smoke — {len(all_steps())} steps on "
          f"{', '.join(label for label, _ in boots)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
