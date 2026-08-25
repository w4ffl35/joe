#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# run-llm-smoke.sh — Phase 2d-4 end-to-end LLM smoke gate (GitHub issue #8).
#
# Proves the FULL locked marker sequence (docs/phase2d-wire.md §5):
#   NET: 1, NET: 2, NET: 3, ARP: 1, TCP: 1, SND: 36, RCV: 36,
#   JSON: 1, TOOL: 2, LLM: 1, Hello World from JOE!
#
# That is the kernel -> host stub -> JSON parse -> tool-queue enqueue -> ack
# round-trip, deterministic and timeout-safe. A "JSON: E<code>" marker fails
# the gate.
#
# How it works (mirrors qemu-net-smoke's harness pattern):
#   1. Start the deterministic Python stub (scripts/llm_stub_server.py) on
#      port 8080 (bound 0.0.0.0, so QEMU user-net's gateway alias 10.0.2.2
#      reaches host loopback — no hostfwd needed for kernel -> host).
#   2. Boot the GRUB/ISO kernel (kernel-grub.elf, NIC compiled in — the PVH
#      path has no legacy PCI config space, so the NIC only runs here) with:
#        -netdev user,id=n0 -device virtio-net-pci,disable-modern=on,netdev=n0
#      Serial captured to build/serial-llm.log, -display none, timeout-bounded.
#   3. Assert the serial log contains the FULL ORDERED marker sequence.
#
# The real llama.cpp variant (documented, not CI-gated): run
#   LLM_SERVER=llama-server make qemu-llm-smoke
# (or start `llama-server -m <model> --port 8080` yourself and set
# LLM_SERVER=skip — the stub is then not started and the same gate runs
# against the real server).
#
# Port is LOCKED at 8080 (wire doc + kernel HOST_PORT): there is no LLM_PORT
# override — the kernel's TCP stack hardcodes 8080, so a different stub port
# could never be reached by the guest.
#
# Routing (LOCKED, docs/phase2d-wire.md §1): QEMU user-net gives the guest
# 10.0.2.15/24 with gateway 10.0.2.2; slirp's 10.0.2.2 gateway alias reaches
# host loopback services directly, so the stub on 127.0.0.1:8080 is reachable
# from the guest at 10.0.2.2:8080. No hostfwd (it would forward host -> guest,
# the wrong direction).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/joeos-net.iso"
SERIAL_LOG="$BUILD/serial-llm.log"
STUB_LOG="$BUILD/llm-stub.log"
STUB_PID_FILE="$BUILD/llm-stub.pid"
# Port is LOCKED at 8080 by the wire doc — the kernel's TCP stack hardcodes
# HOST_PORT 8080 (kernel/net_stack.c) and the Curlee bridge calls
# net_connect(8080), so the stub MUST bind 8080 for the guest's connection to
# reach it. There is deliberately NO LLM_PORT override (it could never work).
PORT="8080"
QEMU_TIMEOUT="${QEMU_LLM_TIMEOUT:-30}"

# The LOCKED ordered marker sequence (docs/phase2d-wire.md §5). The gate
# asserts this EXACT order; a missing or reordered marker fails.
#
# Single-line pattern with literal \n escapes (NOT a multi-line string): grep
# -P treats each line of a multi-line pattern argument as a SEPARATE pattern
# ("-P only supports a single pattern") and would never match the ordered
# sequence. The escaped form is exactly what the existing qemu-loop-smoke
# target uses (grep -Pzo 'FR:0\nFR:1\n...').
#
# On the GRUB/fb boot path the deterministic 60 FPS loop (FR:0..FR:3, RING: 1,
# FB: 1) runs AFTER the LLM round-trip and before the serial tail, so the
# pattern allows the loop markers between "LLM: 1" and "Hello World from JOE!"
# (the wire doc's tail follows the display branch, not immediately after the
# LLM ack). The ordered part that MUST be contiguous is the LLM round-trip
# itself (NET: 1 .. LLM: 1); the tail is checked last.
#
# The 2d-1 RX marker ("RX: <len>", the slirp ARP reply consumed by
# net_bringup) is emitted between "NET: 3" and the LLM round-trip, so the
# pattern allows it there ("NET: 3\nRX: <2 digits>\nARP: 1").
EXPECTED_SEQUENCE='NET: 1\nNET: 2\nNET: 3\nRX: [0-9][0-9]\nARP: 1\nTCP: 1\nSND: 36\nRCV: 36\nJSON: 1\nTOOL: 2\nLLM: 1'
TAIL_SEQUENCE='Hello World from JOE!'

if [[ ! -f "$ISO" ]]; then
    echo "FAIL: $ISO missing — build it first (make qemu-llm-smoke builds it via the Makefile dependency)" >&2
    exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found" >&2
    exit 1
fi

# Clean stale artifacts from an interrupted run.
rm -f "$SERIAL_LOG" "$STUB_LOG" "$STUB_PID_FILE"
# Free the stub port from any stale listener (e.g. an interrupted run).
if command -v fuser >/dev/null 2>&1; then
    fuser -k "${PORT}/tcp" 2>/dev/null || true
fi
sleep 1

# ---------------------------------------------------------------------------
# 1. Host server: the deterministic stub (or the real llama.cpp variant).
# ---------------------------------------------------------------------------
# LLM_SERVER values:
#   (unset/empty) -> python3 scripts/llm_stub_server.py (CI-safe default)
#   skip          -> do NOT start anything; a server is already running
#                    (e.g. `llama-server -m <model> --port 8080`)
#   <command>     -> start that command instead (e.g. LLM_SERVER=llama-server
#                    with the model configured via the command's own flags;
#                    the documented real-LLM variant)
STUB_PID=""
if [[ -z "${LLM_SERVER:-}" ]]; then
    if ! command -v python3 >/dev/null 2>&1; then
        echo "FAIL: python3 not found (needed for scripts/llm_stub_server.py)" >&2
        exit 1
    fi
    # The stub MUST own the port — if anything else is already listening (e.g.
    # a root-owned llama.cpp/AIRunner server on :8080, common on dev hosts),
    # the guest's request would go to THAT server (wrong response body -> gate
    # fails with JSON: E3). Fail fast instead of silently testing the wrong
    # server. (fuser -k above only works for our own processes; a foreign
    # listener is detected here.)
    if command -v ss >/dev/null 2>&1; then
        if ss -tlnp 2>/dev/null | grep -q ":${PORT} " ; then
            echo "FAIL: port $PORT is already in use by another process." >&2
            echo "      The stub must own the port (the gate tests OUR stub)." >&2
            echo "      Port 8080 is LOCKED by the wire doc (the kernel hardcodes" >&2
            echo "      HOST_PORT 8080), so the conflicting service must be freed" >&2
            echo "      or moved for the gate to run (CI has no such service)." >&2
            exit 1
        fi
    fi
    echo "llm-smoke: starting deterministic stub (python3 scripts/llm_stub_server.py --port $PORT)"
    python3 "$ROOT/scripts/llm_stub_server.py" --host 0.0.0.0 --port "$PORT" --log "$STUB_LOG" \
        >"$BUILD/llm-stub.stdout" 2>&1 &
    STUB_PID=$!
    echo "$STUB_PID" > "$STUB_PID_FILE"
    # Give the stub a moment to bind; fail fast if it died (e.g. bind error).
    sleep 1
    if ! kill -0 "$STUB_PID" 2>/dev/null; then
        echo "FAIL: llm stub exited immediately (port conflict? see $BUILD/llm-stub.stdout)" >&2
        cat "$BUILD/llm-stub.stdout" >&2 || true
        exit 1
    fi
elif [[ "$LLM_SERVER" == "skip" ]]; then
    echo "llm-smoke: LLM_SERVER=skip — using an already-running host server on :$PORT"
else
    echo "llm-smoke: starting host server: $LLM_SERVER"
    $LLM_SERVER >"$BUILD/llm-stub.stdout" 2>&1 &
    STUB_PID=$!
    echo "$STUB_PID" > "$STUB_PID_FILE"
fi

cleanup() {
    if [[ -n "$STUB_PID" ]] && kill -0 "$STUB_PID" 2>/dev/null; then
        kill "$STUB_PID" 2>/dev/null || true
        wait "$STUB_PID" 2>/dev/null || true
    fi
    rm -f "$STUB_PID_FILE"
}
trap cleanup EXIT

# Give the stub a moment to bind before QEMU's first ARP/TCP attempt.
sleep 1

# ---------------------------------------------------------------------------
# 2. QEMU launch: GRUB/ISO path with the NIC (user-net, NO hostfwd).
# ---------------------------------------------------------------------------
echo "llm-smoke: booting $ISO with -netdev user,id=n0 (guest 10.0.2.15 -> gw 10.0.2.2 -> host :$PORT)"
set +e
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -boot d -no-reboot \
    -netdev user,id=n0 \
    -device virtio-net-pci,disable-modern=on,netdev=n0 \
    -serial file:"$SERIAL_LOG" \
    -display none >"$BUILD/llm-qemu.stdout" 2>&1
QEMU_RC=$?
set -e
# QEMU exits 124 on timeout (the expected path: the kernel halts, QEMU idles
# until timeout kills it) — anything else is a launch failure.
if [[ $QEMU_RC -ne 0 && $QEMU_RC -ne 124 ]]; then
    echo "FAIL: QEMU exited with $QEMU_RC (launch error?) — stderr:" >&2
    tail -20 "$BUILD/llm-qemu.stdout" >&2 || true
    exit 1
fi

if [[ ! -s "$SERIAL_LOG" ]]; then
    echo "FAIL: serial log empty — did the kernel boot? ($SERIAL_LOG)" >&2
    echo "qemu stderr: $(tail -20 "$BUILD/llm-qemu.stdout" 2>/dev/null)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 3. Assertions: the FULL ordered marker sequence.
# ---------------------------------------------------------------------------
# Fail fast on a JSON parse error: "JSON: E<code>" (any code) is a hard gate
# failure per acceptance criterion 1.
if grep -qE 'JSON: E[0-9]' "$SERIAL_LOG"; then
    echo "FAIL: JSON parse error marker present in serial log (JSON: E<code> fails the gate)" >&2
    echo "serial log: $(cat "$SERIAL_LOG")" >&2
    exit 1
fi

# The ordered round-trip sequence: grep -Pzo treats the log as a single string
# and matches the markers in exact order (same pattern as qemu-loop-smoke). The
# expected sequence has no regex metacharacters (no '.', '?', etc.), so the
# literal text doubles as a PCRE pattern safely.
if grep -Pzo "$EXPECTED_SEQUENCE" "$SERIAL_LOG" > /dev/null; then
    echo "PASS: full LLM round-trip marker sequence in order (NET: 1 .. LLM: 1)"
else
    echo "FAIL: serial log does not contain the full ordered LLM round-trip sequence" >&2
    echo "expected:" >&2
    echo "$EXPECTED_SEQUENCE" >&2
    echo "serial log: $(cat "$SERIAL_LOG")" >&2
    exit 1
fi

# The serial tail (existing acceptance gate): must appear anywhere after the
# LLM ack (the display loop markers FR:/RING:/FB: may run in between).
if grep -q "$TAIL_SEQUENCE" "$SERIAL_LOG"; then
    echo "PASS: serial tail (Hello World from JOE!) present"
    echo "serial log: $(cat "$SERIAL_LOG")"
else
    echo "FAIL: serial tail missing (kernel did not reach the halt path)" >&2
    echo "serial log: $(cat "$SERIAL_LOG")" >&2
    exit 1
fi

if [[ -f "$STUB_LOG" ]]; then
    echo "stub log: $(cat "$STUB_LOG")"
fi
