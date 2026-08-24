#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# Locate the curlee compiler binary.
#
# Resolution order:
#   1. $CURLEE (explicit path to the curlee executable)
#   2. $CURLEE_ROOT/bin/curlee (explicit root)
#   3. $HOME/Projects/curlee/build/linux-debug/curlee (default dev location)
#   4. curlee on $PATH
#
# Prints the resolved path to stdout (or exits non-zero with a message on
# stderr if not found).
set -euo pipefail

curlee_bin=""
if [[ -n "${CURLEE:-}" ]]; then
    curlee_bin="${CURLEE}"
elif [[ -n "${CURLEE_ROOT:-}" ]]; then
    curlee_bin="${CURLEE_ROOT}/bin/curlee"
elif [[ -x "${HOME}/Projects/curlee/build/linux-debug/curlee" ]]; then
    curlee_bin="${HOME}/Projects/curlee/build/linux-debug/curlee"
elif command -v curlee >/dev/null 2>&1; then
    curlee_bin="$(command -v curlee)"
fi

if [[ -z "${curlee_bin}" ]]; then
    echo "error: curlee compiler not found. Set CURLEE_ROOT (or CURLEE) or install curlee on PATH." >&2
    exit 1
fi

echo "${curlee_bin}"
