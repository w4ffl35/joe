#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# run-fb-pixels-smoke.sh — the framebuffer PIXEL gate.
#
# Boots the FB-mode ISO, waits for the deterministic 4-frame loop to
# reach its halt path, takes a QEMU monitor (QMP) screendump of the live
# display, and checks the colour of pixels render_frame
# (kernel/kernel.curlee) is known to draw for the LAST rendered frame
# (loop index 3, panel_y=100 — render_frame's panel_parity math):
#   (150,150) panel interior, near the panel's own top-left corner
#   (200,170) on the line drawn OVER the panel (line wins; exercises x AND y)
#   (230,210) panel interior again, close to the panel's FAR corner (large
#             x AND y) — a stride/scale error that only misbehaves once x
#             or y grows cannot coincidentally agree with (150,150) too
#   (300,300) plain background, outside every shape (checks the fix does
#             not bleed a write into neighbouring, untouched pixels)
# None of these sit on the "JOE" glyph text (the glyph set is
# incomplete, so text pixels are not a reliable oracle).
#
# QMP (not the legacy HMP monitor) is used because it is trivially
# scriptable with Python's stdlib json + socket (no expect/socat dependency
# needed); the actual screendump + PPM colour check is
# scripts/qemu_fb_pixel_check.py.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/joeos-fb.iso"
SERIAL_LOG="$BUILD/serial-fb-pixels.log"
QEMU_LOG="$BUILD/fb-pixels-qemu.stdout"
QMP_SOCK="$BUILD/fb-pixels-qmp.sock"
PPM="$BUILD/fb-pixels.ppm"
QEMU_TIMEOUT="${QEMU_FB_PIXELS_TIMEOUT:-25}"

CHECKS=(
    "150,150,220,80,40"   # panel interior, near corner: rgb(220,80,40)
    "200,170,40,220,80"   # line drawn over the panel: rgb(40,220,80)
    "230,210,220,80,40"   # panel interior, far corner: rgb(220,80,40)
    "300,300,8,8,12"      # background, outside every shape: rgb(8,8,12)
)

if [[ ! -f "$ISO" ]]; then
    echo "FAIL: $ISO missing — build it first (make iso-fb)" >&2
    exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 not found (needed for the QMP screendump check)" >&2
    exit 1
fi

rm -f "$SERIAL_LOG" "$QEMU_LOG" "$QMP_SOCK" "$PPM"

echo "fb-pixels-smoke: booting $ISO (QMP screendump gate)"
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom "$ISO" -boot d -no-reboot -net none \
    -vga std \
    -serial file:"$SERIAL_LOG" \
    -qmp "unix:$QMP_SOCK,server=on,wait=off" \
    -display none >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

deadline=$((SECONDS + QEMU_TIMEOUT))

# Wait for the QMP socket to appear.
while [[ ! -S "$QMP_SOCK" ]]; do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "FAIL: QEMU exited before the QMP socket appeared" >&2
        cat "$QEMU_LOG" >&2 || true
        exit 1
    fi
    if [[ $SECONDS -ge $deadline ]]; then
        echo "FAIL: timed out waiting for the QMP socket" >&2
        exit 1
    fi
    sleep 0.2
done

# Wait for the kernel to reach its normal halt path — the 4-frame loop
# (FR:0..FR:3) has rendered and presented its last frame by then, so the
# screendump is deterministic (no race with an in-progress draw).
while ! grep -q 'Hello World from JOE' "$SERIAL_LOG" 2>/dev/null; do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "FAIL: QEMU exited before the kernel reached its halt path" >&2
        cat "$SERIAL_LOG" >&2 || true
        exit 1
    fi
    if [[ $SECONDS -ge $deadline ]]; then
        echo "FAIL: timed out waiting for the kernel halt marker" >&2
        echo "serial log: $(cat "$SERIAL_LOG" 2>/dev/null)" >&2
        exit 1
    fi
    sleep 0.2
done

check_args=()
for c in "${CHECKS[@]}"; do
    check_args+=(--check "$c")
done

if python3 "$ROOT/scripts/qemu_fb_pixel_check.py" \
    --qmp-sock "$QMP_SOCK" --ppm-out "$PPM" "${check_args[@]}"; then
    echo "PASS: fb-pixels-smoke — render_frame's pixels match at $PPM"
else
    echo "FAIL: fb-pixels-smoke — pixel mismatch (see above)" >&2
    exit 1
fi
