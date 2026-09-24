#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# run-pace-smoke.sh — the frame pacing gate.
#
# Boots apps/pace (bundled by `make qemu-pace-smoke`) and measures its
# frame rate from the host: scripts/qemu_pace_check.py timestamps each
# "PACE: <nnn>" serial line as it arrives and requires 60 fps +-1% over
# the 600 frames (10 s). Every combination of
#
#   accelerator  kvm (-accel kvm, skipped when /dev/kvm is unusable) and
#                tcg (-accel tcg, pure emulation)
#   boot path    pvh (build/kernel-smoke.elf, -kernel, 64-bit code) and
#                iso (build/joeos.iso via GRUB, 32-bit code where 64-bit
#                arithmetic goes through helper routines)
#
# is run, and each measured rate is printed. QEMU's PIT follows the host
# clock, so a slow TCG run shows up as a wrong rate rather than as a
# different tick length. The gate fails if any run is outside the limit;
# the tolerance is not adjusted per accelerator.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
FRAMES="${PACE_FRAMES:-600}"
HZ="${PACE_HZ:-60}"
TOLERANCE_PCT="${PACE_TOLERANCE_PCT:-1.0}"
TIMEOUT="${PACE_TIMEOUT:-40}"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found" >&2
    exit 1
fi
for image in "$BUILD/kernel-smoke.elf" "$BUILD/joeos.iso"; do
    if [[ ! -f "$image" ]]; then
        echo "FAIL: $image missing — run \`make qemu-pace-smoke\`" >&2
        exit 1
    fi
done

QEMU_COMMON=(-display none -monitor none -no-reboot -net none
             -serial stdio)
BOOT_PVH=(-kernel "$BUILD/kernel-smoke.elf")
BOOT_ISO=(-cdrom "$BUILD/joeos.iso" -boot d)
failures=0

run_one() {
    local accel="$1" boot="$2"
    local -n boot_args="BOOT_${boot^^}"
    if python3 "$ROOT/scripts/qemu_pace_check.py" \
        --label "$accel/$boot" --frames "$FRAMES" --hz "$HZ" \
        --tolerance-pct "$TOLERANCE_PCT" --timeout "$TIMEOUT" -- \
        qemu-system-x86_64 -accel "$accel" "${QEMU_COMMON[@]}" \
        "${boot_args[@]}"; then
        return 0
    fi
    failures=$((failures + 1))
}

for accel in kvm tcg; do
    if [[ "$accel" == kvm && ! ( -r /dev/kvm && -w /dev/kvm ) ]]; then
        echo "SKIP: kvm runs — /dev/kvm is missing or not accessible"
        continue
    fi
    for boot in pvh iso; do
        run_one "$accel" "$boot"
    done
done

if [[ "$failures" -ne 0 ]]; then
    echo "FAIL: pace-smoke — $failures run(s) outside +-${TOLERANCE_PCT}%" >&2
    exit 1
fi
echo "PASS: pace-smoke — every run within +-${TOLERANCE_PCT}% of ${HZ} fps"
