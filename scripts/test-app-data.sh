#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# test-app-data.sh — APP_DATA handling in scripts/build-kernel.sh: data
# files land next to the merged output byte for byte, a missing file and
# two files of one name are build errors, and no APP_DATA copies nothing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d)"
trap 'rm -rf -- "$tmpdir"' EXIT

mkdir -p "$tmpdir/a" "$tmpdir/b"
printf 'first array' > "$tmpdir/a/tiles.npy"
printf 'second array' > "$tmpdir/a/clips.npy"
printf 'another tiles' > "$tmpdir/b/tiles.npy"

merge() {
    local out_dir="$1"
    shift
    env "$@" bash "$ROOT/scripts/build-kernel.sh" "$out_dir/kernel-merged.curlee"
}

merge "$tmpdir/plain" APP_DATA= >/dev/null
if [[ "$(ls "$tmpdir/plain")" != "kernel-merged.curlee" ]]; then
    echo "FAIL: no APP_DATA still copied files: $(ls "$tmpdir/plain")" >&2
    exit 1
fi

merge "$tmpdir/out" APP_DATA="$tmpdir/a/tiles.npy $tmpdir/a/clips.npy" \
    >/dev/null
cmp -s "$tmpdir/a/tiles.npy" "$tmpdir/out/tiles.npy"
cmp -s "$tmpdir/a/clips.npy" "$tmpdir/out/clips.npy"

if merge "$tmpdir/missing" APP_DATA="$tmpdir/a/nothing.npy" \
    >"$tmpdir/missing.log" 2>&1; then
    echo "FAIL: a missing APP_DATA file was accepted" >&2
    exit 1
fi
grep -q "nothing.npy" "$tmpdir/missing.log"

if merge "$tmpdir/twice" \
    APP_DATA="$tmpdir/a/tiles.npy $tmpdir/b/tiles.npy" \
    >"$tmpdir/twice.log" 2>&1; then
    echo "FAIL: two APP_DATA files of one name were accepted" >&2
    exit 1
fi
grep -q "tiles.npy twice" "$tmpdir/twice.log"

echo "APP-DATA-TEST: PASS copy, no-data, missing-file and duplicate-name cases"
