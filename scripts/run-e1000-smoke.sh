#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# run-e1000-smoke.sh — Workstream C (fabrication-fix plan): the REAL Intel
# e1000 acceptance gate.
#
# The fabrication report's real-world test was a session asked to write an
# e1000 driver against real QEMU hardware; it fabricated a serial log excerpt
# ("E1000: 1 / E1000: 2") and claimed the gate passed without the driver ever
# being merged or the target existing. This gate is the checked-in, rerunnable
# artifact that makes that exact fabrication structurally impossible: the
# harness (Workstream A, ~/Projects/headlesscode) re-runs THIS script, and it
# can only PASS on real hardware with the real serial markers in order.
#
# What it proves (the scoped milestone):
#   E1000: 1  an Intel e1000 (vendor 0x8086, device 0x100E) was detected via
#             legacy PCI enumeration on real QEMU hardware
#   E1000: 2  the device was reset (CTRL.RST written through the MMIO BAR0
#             window, readback confirmed: RST consumed, STATUS.LU set)
#
# How it works (mirrors run-llm-smoke.sh's harness pattern):
#   1. Boot the GRUB/ISO kernel (kernel-grub.elf, e1000 driver compiled in —
#      the PVH `-kernel` path has NO legacy PCI config space, so the NIC only
#      runs where SeaBIOS runs: the ISO boot) with:
#        -netdev user,id=n0 -device e1000,netdev=n0
#      Serial captured to build/serial-e1000.log, -display none, timeout-
#      bounded. The kernel halts deterministically; QEMU idles until the
#      timeout kills it (the expected path, RC 124 — same as run-llm-smoke).
#   2. Assert the serial log contains the ORDERED marker sequence
#      "E1000: 1\nE1000: 2" (grep -Pzo — the qemu-loop-smoke precedent).
#      Missing, reordered, or truncated markers FAIL the gate.
#
# Fail-closed: set -euo pipefail; stale artifacts cleaned first; the ISO must
# exist (the Makefile target builds it); qemu must be present; non-empty
# serial log required; exit code non-zero on any failure.
#
# Deterministic + rerunnable: no host services, no ports, no network traffic
# (user-net is just a NIC backend — the milestone never touches TX/RX), so
# repeated runs are stable.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/joeos-net.iso"
SERIAL_LOG="$BUILD/serial-e1000.log"
QEMU_TIMEOUT="${QEMU_E1000_TIMEOUT:-25}"

# The LOCKED ordered marker sequence. Single-line pattern with literal \n
# escapes (NOT a multi-line string): grep -P treats each line of a multi-line
# pattern argument as a SEPARATE pattern and would never match the ordered
# sequence (the qemu-loop-smoke precedent, run-llm-smoke.sh's comment). The
# pattern has no regex metacharacters besides the escapes, so the literal
# text doubles as a PCRE pattern safely.
EXPECTED_SEQUENCE='E1000: 1\nE1000: 2'

if [[ ! -f "$ISO" ]]; then
    echo "FAIL: $ISO missing — build it first (make qemu-e1000-smoke builds it via the Makefile dependency)" >&2
    exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found" >&2
    exit 1
fi

# Clean stale artifacts from an interrupted run (deterministic rerunnability).
rm -f "$SERIAL_LOG"

# ---------------------------------------------------------------------------
# 1. QEMU launch: GRUB/ISO path with the real Intel e1000 NIC (user-net).
#    The e1000 driver is compiled into kernel-grub.elf (the ISO), so the NIC
#    is only reachable here — NOT on the PVH `-kernel` path, which has no
#    legacy PCI config space (docs/phase2f-report.md §4).
# ---------------------------------------------------------------------------
echo "e1000-smoke: booting $ISO with -device e1000 (real Intel NIC, user-net backend)"
set +e
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -boot d -no-reboot \
    -netdev user,id=n0 \
    -device e1000,netdev=n0 \
    -serial file:"$SERIAL_LOG" \
    -display none >"$BUILD/e1000-qemu.stdout" 2>&1
QEMU_RC=$?
set -e
# QEMU exits 124 on timeout (the expected path: the kernel halts, QEMU idles
# until timeout kills it) — anything else is a launch failure.
if [[ $QEMU_RC -ne 0 && $QEMU_RC -ne 124 ]]; then
    echo "FAIL: QEMU exited with $QEMU_RC (launch error?) — stderr:" >&2
    tail -20 "$BUILD/e1000-qemu.stdout" >&2 || true
    exit 1
fi

if [[ ! -s "$SERIAL_LOG" ]]; then
    echo "FAIL: serial log empty — did the kernel boot? ($SERIAL_LOG)" >&2
    echo "qemu stderr: $(tail -20 "$BUILD/e1000-qemu.stdout" 2>/dev/null)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Assertions: the ordered marker sequence.
# ---------------------------------------------------------------------------
# grep -Pzo treats the log as a single string and matches the markers in
# exact order (same pattern as qemu-loop-smoke). Missing or reordered
# markers fail closed.
if grep -Pzo "$EXPECTED_SEQUENCE" "$SERIAL_LOG" > /dev/null; then
    echo "PASS: Intel e1000 detected + reset on real QEMU hardware (E1000: 1 -> E1000: 2 in order)"
else
    echo "FAIL: serial log does not contain the ordered e1000 marker sequence" >&2
    echo "expected: E1000: 1, then E1000: 2 (in order)" >&2
    echo "serial log: $(cat "$SERIAL_LOG")" >&2
    exit 1
fi

# The serial tail (existing acceptance gate): the kernel reached the halt
# path after emitting the markers.
if grep -q 'Hello World from JOE' "$SERIAL_LOG"; then
    echo "PASS: serial tail (Hello World from JOE!) present — kernel halted cleanly"
    echo "serial log:"
    cat "$SERIAL_LOG"
else
    echo "FAIL: serial tail missing (kernel did not reach the halt path)" >&2
    echo "serial log: $(cat "$SERIAL_LOG")" >&2
    exit 1
fi
