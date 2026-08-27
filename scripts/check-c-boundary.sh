#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# check-c-boundary.sh — enforce the C boundary policy (docs/c-boundary-policy.md).
#
# Goal: stop the C surface from ballooning. Pure logic and data tables belong
# in the Curlee layer; C is only for I/O touches and raw memory moves.
#
# Checks:
#   1. No kernel/*.c exceeds 200 lines (no grandfathers remain — the last one,
#      libgcc32.c, was deleted in gh issue #21 once curlee #288 bundled the
#      32-bit GCC ABI helpers into the toolchain runtime).
#   2. No kernel/*.c contains a pure-data red flag: a `switch` with >4 cases,
#      or a `static const` array with >32 elements.
#   3. No kernel/*.c calls back into Curlee (calls a curlee_* / fb_* / net_*
#      function that the codegen would emit — the C layer must never call up).
#
# The C-to-Curlee migration is COMPLETE (gh issue #31): kernel/ now holds ZERO
# .c files, so the empty-glob case below is the normal, passing state — a bare
# `for f in kernel/*.c` would expand to the literal pattern and break under
# `set -e`, so the loop is driven from a nullglob array with an explicit
# zero-file early exit.
#
# Reports (warning) per-file line counts so the trend is visible in CI.
#
# Exit 0 on pass, 1 on any violation (so `make c-boundary` fails the build).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="$ROOT/kernel"

# The 200-line cap.
MAX_LINES=200

violations=0

echo "=== C boundary check (docs/c-boundary-policy.md) ==="

shopt -s nullglob
c_files=("$KERNEL_DIR"/*.c)
if (( ${#c_files[@]} == 0 )); then
    echo "  [ok]   no kernel/*.c files — kernel/ is ZERO C files (C-to-Curlee migration complete, gh issue #31)"
    echo "=== C boundary: OK (zero C files; no grandfathered files) ==="
    exit 0
fi

for f in "${c_files[@]}"; do
    name="$(basename "$f")"
    lines="$(wc -l < "$f")"

    # 1. Line cap (no grandfathers — every kernel/*.c must be at or under it).
    if (( lines > MAX_LINES )); then
        echo "  [FAIL] $name: $lines lines exceeds the $MAX_LINES-line cap (docs/c-boundary-policy.md §3)"
        violations=$((violations + 1))
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
    #      (The former 36-byte wire request in net_stack.c migrated to
    #      net_stack.curlee's req_payload_byte in gh issue #12 — no exemption.)
    big_const="$(grep -cE 'static const .*\[[0-9]{2,}\]' "$f" || true)"
    if (( big_const > 0 )); then
        echo "  [FAIL] $name: $big_const large static const array(s) — pure-data table belongs in Curlee"
        violations=$((violations + 1))
    fi

    # 3. C must never call up into Curlee (curlee_* symbols are codegen-emitted
    #    with static linkage; a C call to one is a layering violation).
    #    Only match NON-comment lines (grep -v strips // comments; the former
    #    vbe.c mentioned fb_ready only in its header comment).
    if grep -vE '^\s*//' "$f" | grep -qE '\b(curlee_|fb_init|fb_ready|fb_get_|fb_asset_|fb_ring_|fb_loop_|json_|vga_cell|render_frame|draw_glyph)\s*\('; then
        # The former fb.c whitelist is gone with the file (gh issue #296):
        # kernel/fb.c is deleted, so no C file legitimately defines fb_* /
        # curlee_* symbols — any match is a layering violation.
        echo "  [FAIL] $name: calls a Curlee/codegen symbol (C must not call up — docs/c-boundary-policy.md §2)"
        violations=$((violations + 1))
    fi
done

if (( violations > 0 )); then
    echo "=== C boundary: FAIL ($violations violation(s)) ==="
    exit 1
fi

echo "=== C boundary: OK (no violations; no grandfathered files) ==="
