#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
# Place and verify one opaque blob at a fixed sector offset in a raw disk.

set -euo pipefail

usage() {
    echo "usage: $0 [--verify-only] IMAGE BLOB LBA [SECTOR_SIZE]" >&2
}

verify_only=0
if [[ "${1:-}" == "--verify-only" ]]; then
    verify_only=1
    shift
fi

if (( $# < 3 || $# > 4 )); then
    usage
    exit 2
fi

image=$1
blob=$2
lba=$3
sector_size=${4:-512}

if [[ ! -f "$image" ]]; then
    echo "FAIL: disk image is not a regular file: $image" >&2
    exit 1
fi
if [[ ! -f "$blob" ]]; then
    echo "FAIL: blob is not a regular file: $blob" >&2
    exit 1
fi
if [[ ! "$lba" =~ ^[0-9]+$ || ! "$sector_size" =~ ^[1-9][0-9]*$ ]]; then
    echo "FAIL: LBA must be non-negative and sector size must be positive" >&2
    exit 2
fi
if [[ "$(readlink -f -- "$image")" == "$(readlink -f -- "$blob")" ]]; then
    echo "FAIL: disk image and blob must be different files" >&2
    exit 1
fi

max_int=9223372036854775807
if (( lba > max_int / sector_size )); then
    echo "FAIL: LBA byte offset overflows signed 64-bit arithmetic" >&2
    exit 2
fi

offset=$((lba * sector_size))
image_bytes=$(stat -c '%s' -- "$image")
blob_bytes=$(stat -c '%s' -- "$blob")
if (( blob_bytes == 0 )); then
    echo "FAIL: refusing to place an empty blob" >&2
    exit 1
fi
if (( blob_bytes > max_int - offset )); then
    echo "FAIL: blob end offset overflows signed 64-bit arithmetic" >&2
    exit 2
fi
end=$((offset + blob_bytes))
if (( end > image_bytes )); then
    echo "FAIL: blob range [$offset, $end) exceeds $image_bytes-byte image" >&2
    exit 1
fi

if (( verify_only == 0 )); then
    dd if="$blob" of="$image" bs="$sector_size" seek="$lba" conv=notrunc status=none
fi

# GNU dd's byte-count flags avoid a one-byte block size for large model blobs.
# cmp proves exact read-back without a filesystem or a second blob copy.
if ! cmp -s -n "$blob_bytes" -- "$blob" \
    <(dd if="$image" bs=1M iflag=skip_bytes,count_bytes \
        skip="$offset" count="$blob_bytes" status=none); then
    echo "FAIL: read-back mismatch at LBA $lba" >&2
    exit 1
fi

if (( verify_only == 1 )); then
    action=verified
else
    action=placed
fi
echo "RAW-BLOB: PASS $action $blob_bytes bytes at LBA $lba (byte offset $offset, sector $sector_size)"
