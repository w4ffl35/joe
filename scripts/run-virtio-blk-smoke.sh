#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
# Boot JOE with a raw VirtIO disk and require ordered block-driver evidence.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/joeos-net.iso"
IMAGE="$BUILD/virtio-blk-smoke.raw"
BLOB="$BUILD/virtio-blk-smoke.bin"
SERIAL_LOG="$BUILD/serial-virtio-blk.log"
QEMU_LOG="$BUILD/virtio-blk-qemu.stdout"
QEMU_TIMEOUT="${QEMU_BLK_TIMEOUT:-25}"

if [[ ! -f "$ISO" ]]; then
    echo "FAIL: $ISO is missing; build the smoke ISO first" >&2
    exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found" >&2
    exit 1
fi

rm -f "$IMAGE" "$BLOB" "$SERIAL_LOG" "$QEMU_LOG"
truncate -s $((32 * 512)) "$IMAGE"
truncate -s 1024 "$BLOB"
printf 'JOE' | dd of="$BLOB" conv=notrunc status=none
printf 'A' | dd of="$BLOB" bs=1 seek=511 conv=notrunc status=none
printf 'B' | dd of="$BLOB" bs=1 seek=512 conv=notrunc status=none
printf 'Z' | dd of="$BLOB" bs=1 seek=1023 conv=notrunc status=none
"$ROOT/scripts/place-raw-blob.sh" "$IMAGE" "$BLOB" 7

set +e
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom "$ISO" -boot d -no-reboot -net none \
    -drive if=none,file="$IMAGE",format=raw,id=modeldisk \
    -device virtio-blk-pci,disable-modern=on,drive=modeldisk \
    -serial file:"$SERIAL_LOG" -display none >"$QEMU_LOG" 2>&1
qemu_rc=$?
set -e

if [[ $qemu_rc -ne 0 && $qemu_rc -ne 124 ]]; then
    echo "FAIL: QEMU exited with $qemu_rc" >&2
    tail -20 "$QEMU_LOG" >&2 || true
    exit 1
fi
if [[ ! -s "$SERIAL_LOG" ]]; then
    echo "FAIL: empty serial log" >&2
    tail -20 "$QEMU_LOG" >&2 || true
    exit 1
fi
if ! grep -Pzo 'BLK: 1\nBLK: 2\nBLK: 3' "$SERIAL_LOG" >/dev/null; then
    echo "FAIL: ordered VirtIO block markers are missing" >&2
    echo "serial log: $(tr '\n' ' ' < "$SERIAL_LOG")" >&2
    exit 1
fi
if ! grep -q 'Hello World from JOE' "$SERIAL_LOG"; then
    echo "FAIL: kernel did not reach its normal halt path" >&2
    exit 1
fi

echo "VIRTIO-BLK: PASS PCI init, queue init, and two-sector LBA 7 read"
