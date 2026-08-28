#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# make-blk-disk.sh — build-side mechanism to put a model blob (or the built-in
# smoke test pattern) at a known LBA offset on a raw disk image (gh issue #26,
# acceptance criterion 4). No filesystem — a raw image the virtio-blk driver
# reads sector-by-sector.
#
# Usage:
#   bash scripts/make-blk-disk.sh <out.img> [blob_file] [lba]
#
#   out.img    the raw disk image to create (default 1 MiB = 2048 sectors x
#              512 B, zero-filled)
#   blob_file  optional: a model blob to place at `lba` (the Native Inference
#              use case — the driver's blk_read(lba, n) loads it). When
#              omitted, the built-in "JOE-BLK!" test pattern is written over
#              LBA 64..71 (8 sectors, 4096 B) — the deterministic smoke
#              pattern kernel/blk_bringup (kernel.curlee) verifies.
#   lba        the sector offset for blob_file (default 64). Ignored (fixed
#              at 64) when no blob_file is given.
#
# The default test pattern matches blk_pattern_byte in kernel.curlee EXACTLY:
# byte j of the region = "JOE-BLK!"[j % 8], so the smoke gate verifies all
# 1024 bytes of the read (LBA 64 + 2 sectors) against the build-time pattern.
#
# The image is a fixed 1 MiB so the smoke geometry is deterministic; a model
# blob larger than 1 MiB must be given its own image size (extend the dd
# below if the Native Inference milestone needs it).

set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "usage: $0 <out.img> [blob_file] [lba]" >&2
    exit 2
fi

out_img="$1"
blob_file="${2:-}"
lba="${3:-64}"

# 1 MiB zero-filled image (2048 sectors x 512 B).
dd if=/dev/zero of="$out_img" bs=1048576 count=1 status=none

if [[ -n "$blob_file" ]]; then
    # Place the model blob at the requested LBA (byte offset lba * 512).
    if [[ ! -f "$blob_file" ]]; then
        echo "make-blk-disk: error: blob file not found: $blob_file" >&2
        exit 1
    fi
    if [[ ! "$lba" =~ ^[0-9]+$ ]]; then
        echo "make-blk-disk: error: lba must be a non-negative integer: $lba" >&2
        exit 1
    fi
    dd if="$blob_file" of="$out_img" bs=512 seek="$lba" conv=notrunc status=none
    echo "make-blk-disk: wrote $blob_file at LBA $lba -> $out_img"
else
    # Built-in smoke pattern: "JOE-BLK!" repeated over LBA 64..71 (8 sectors,
    # 4096 bytes). 8 pattern bytes x 512 repeats = 4096 bytes exactly.
    pattern=""
    for ((k = 0; k < 512; k++)); do
        pattern+="JOE-BLK!"
    done
    printf '%s' "$pattern" | dd of="$out_img" bs=512 seek=64 conv=notrunc status=none
    echo "make-blk-disk: wrote the JOE-BLK! test pattern over LBA 64..71 -> $out_img"
fi
