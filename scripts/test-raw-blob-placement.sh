#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d)"
trap 'rm -rf -- "$tmpdir"' EXIT

image="$tmpdir/model.raw"
blob="$tmpdir/model.bin"
prefix="$tmpdir/zero-prefix.bin"
sector_size=512
lba=7
offset=$((lba * sector_size))

truncate -s $((32 * sector_size)) "$image"
printf '\x4a\x4f\x45\x00\xffCURLEE-SNN\x7f\x01' > "$blob"
truncate -s "$offset" "$prefix"

before_hash="$(sha256sum "$image" | cut -d' ' -f1)"
"$ROOT/scripts/place-raw-blob.sh" "$image" "$blob" "$lba" "$sector_size"
after_hash="$(sha256sum "$image" | cut -d' ' -f1)"
if [[ "$before_hash" == "$after_hash" ]]; then
    echo "FAIL: placement did not change the image" >&2
    exit 1
fi

cmp -s "$prefix" <(dd if="$image" bs=1 count="$offset" status=none)
blob_bytes="$(stat -c '%s' "$blob")"
cmp -s -n "$blob_bytes" "$blob" \
    <(dd if="$image" bs=1M iflag=skip_bytes,count_bytes \
        skip="$offset" count="$blob_bytes" status=none)
"$ROOT/scripts/place-raw-blob.sh" --verify-only "$image" "$blob" "$lba" "$sector_size"

stable_hash="$(sha256sum "$image" | cut -d' ' -f1)"
if "$ROOT/scripts/place-raw-blob.sh" "$image" "$blob" 32 "$sector_size" >/dev/null 2>&1; then
    echo "FAIL: out-of-capacity placement unexpectedly succeeded" >&2
    exit 1
fi
if [[ "$stable_hash" != "$(sha256sum "$image" | cut -d' ' -f1)" ]]; then
    echo "FAIL: rejected placement modified the image" >&2
    exit 1
fi

echo "RAW-BLOB-TEST: PASS exact placement, read-back, prefix preservation, and capacity rejection"
