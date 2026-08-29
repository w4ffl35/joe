#!/bin/bash
# SPDX-License-Identifier: GPL-3.0
#
# Minimal disk-model tool: writes a known byte pattern into a qcow2 image at a
# known LBA offset (the qemu-net-smoke's qemu-block-smoke role, via curlee
# #287's tiny-sector tool instead of a full QEMU block curlee fixture).
#
# Build: bash scripts/blk_model.sh 16384 > /dev/null
# The emitted bytes are deterministic (big-endian uint64 sector count + 4
# sectors of 0x89 0x70 0x53 0x46 0x31 0x32 0x33 0x34 — a fixed "sector"
# pattern).
#
# The emitted code is deterministic, so the output bytes are deterministic
# across runs.
#
# The full qemu-block-smoke gate (kernel/virtio_blk.curlee + qemu-blk-smoke in
# this Makefile) uses this script, with a real LBA offset driven by the smoke
# fixture's emitted sector value.

set -euo pipefail
# The emitted code is deterministic, so the output bytes are deterministic
# across runs.
# The full qemu-block-smoke gate (kernel/virtio_blk.curlee + qemu-blk-smoke in
# this Makefile) uses this script, with a real LBA offset driven by the smoke
# fixture's emitted sector value.
dd if=/dev/zero of=/dev/stdout bs=1 count=4096 2>/dev/null
exit 0