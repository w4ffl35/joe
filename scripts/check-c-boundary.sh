#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# check-c-boundary.sh — enforce the C boundary policy (docs/c-boundary-policy.md).
#
# Goal: stop the C surface from ballooning. Pure logic and data tables belong
# in the Curlee layer; C is only for I/O touches and raw memory moves.
#
# Checks:
#   1. No kernel/*.c exceeds 200 lines (grandfathered files are exempt and
#      tracked for migration in the policy doc).
#   2. No kernel/*.c contains a pure-data red flag: a `switch` with >4 cases,
#      or a `static const` array with >32 elements.
#   3. No kernel/*.c calls back into Curlee (calls a curlee_* / fb_* / net_*
#      function that the codegen would emit — the C layer must never call up).
#
# Reports (warning) per-file line counts so the trend is visible in CI.
#
# Exit 0 on pass, 1 on any violation (so `make c-boundary` fails the build).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="$ROOT/kernel"

# The 200-line cap.
MAX_LINES=200

# Grandfathered files (tracked in docs/c-boundary-policy.md §3): existing
# drivers that predate the policy. They are exempt from the line cap until
# migration (Curlee gains assignment + bitwise + port I/O).
GRANDFATHERED=(
  "net_stack.c"   # 1056 lines — protocol logic, migrate with assignment+bitwise
  "virtio_net.c"  # 798 lines — ring math, migrate with assignment+port I/O
  "fb.c"          # ~680 lines — blitter loops, migrate with assignment
  "mb2.c"         # 116 lines — tag walk, migrate with assignment+bitwise
  "libgcc32.c"    # 322 lines — GCC ABI shim, NEVER migrates
)

is_grandfathered() {
    local f="$1"
    for g in "${GRANDFATHERED[@]}"; do
        [[ "$f" == "$g" ]] && return 0
    done
    return 1
}

violations=0

echo "=== C boundary check (docs/c-boundary-policy.md) ==="

for f in "$KERNEL_DIR"/*.c; do
    name="$(basename "$f")"
    lines="$(wc -l < "$f")"

    # 1. Line cap (grandfathered exempt).
    if (( lines > MAX_LINES )); then
        if is_grandfathered "$name"; then
            echo "  [warn] $name: $lines lines (grandfathered, exempt from cap — migrate per policy)"
        else
            echo "  [FAIL] $name: $lines lines exceeds the $MAX_LINES-line cap (docs/c-boundary-policy.md §3)"
            violations=$((violations + 1))
        fi
    else
        echo "  [ok]   $name: $lines lines"
    fi

    # 2. Pure-data red flags.
    #    - A `switch` with >4 cases is almost always a lookup table (glyph_row
    #      was exactly this before the refactor).
    switch_cases="$(grep -cE '^\s*case ' "$f" || true)"
    if (( switch_cases > 4 )); then
        echo "  [FAIL] $name: $switch_cases switch cases — pure-data table belongs in Curlee (glyphs.curlee pattern)"
        violations=$((violations + 1))
    fi
    #    - A `static const` array with >32 elements is a data table in C.
    #      Exempt: the 36-byte wire request in net_stack.c (the LOCKED HTTP
    #      payload — a wire-shape constant, not a lookup table).
    big_const="$(grep -cE 'static const .*\[[0-9]{2,}\]' "$f" || true)"
    if (( big_const > 0 )); then
        if [[ "$name" != "net_stack.c" ]]; then
            echo "  [FAIL] $name: $big_const large static const array(s) — pure-data table belongs in Curlee"
            violations=$((violations + 1))
        fi
    fi

    # 3. C must never call up into Curlee (curlee_* symbols are codegen-emitted
    #    with static linkage; a C call to one is a layering violation).
    #    Only match NON-comment lines (grep -v strips // comments; the former
    #    vbe.c mentioned fb_ready only in its header comment).
    if grep -vE '^\s*//' "$f" | grep -qE '\b(curlee_|fb_init|fb_ready|fb_get_|fb_asset_|fb_ring_|fb_loop_|json_|vga_cell|render_frame|draw_glyph)\s*\('; then
        # Whitelist: these extern-implementation shims legitimately DEFINE the
        # symbols (fb_* in fb.c) rather than call them. net_* files only define
        # net_* — they never call them, so they are excluded from the pattern.
        if [[ "$name" != "fb.c" ]]; then
            echo "  [FAIL] $name: calls a Curlee/codegen symbol (C must not call up — docs/c-boundary-policy.md §2)"
            violations=$((violations + 1))
        fi
    fi
done

if (( violations > 0 )); then
    echo "=== C boundary: FAIL ($violations violation(s)) ==="
    exit 1
fi

echo "=== C boundary: OK (no violations; grandfather list: ${#GRANDFATHERED[@]} files) ==="
