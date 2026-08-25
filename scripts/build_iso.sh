#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# build_iso.sh — package a GRUB-bootable ISO for VirtualBox / QEMU.
#
# Usage:
#   bash scripts/build_iso.sh <kernel.elf> <out.iso> [text|fb]
#     text (default): GRUB stays in TEXT mode — the kernel's VGA text buffer
#       (0xB8000) path. Phase 1 / VirtualBox compatibility.
#     fb: GRUB enables gfxterm + a linear framebuffer (VBE 640x480x32) and
#       passes the framebuffer info via the multiboot2 info structure (which
#       the kernel's boot.S now saves; mb2.c parses it — Phase 2e). The
#       kernel's software renderer draws the demo scene.
#
# The kernel ELF must contain a multiboot2 header (kernel/boot.S provides it
# via the GRUB-path link in the Makefile). grub-mkrescue wraps the ELF and a
# grub.cfg into an El Torito ISO that VirtualBox (and qemu -cdrom) can boot:
# GRUB loads the ELF, finds the multiboot2 header, and enters the kernel's
# 64-bit entry, which calls curlee_main.
#
# Serial (COM1) remains enabled for host-side verification in both modes.
#
# Requires: grub-mkrescue (from grub-pc-bin / grub2-common on Debian/Ubuntu).
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 <kernel.elf> <out.iso> [text|fb]" >&2
    exit 2
fi
kernel_elf="$1"
out_iso="$2"
mode="${3:-text}"
if [[ "$mode" != "text" && "$mode" != "fb" ]]; then
    echo "error: mode must be 'text' or 'fb'" >&2
    exit 2
fi

if ! command -v grub-mkrescue >/dev/null 2>&1; then
    echo "error: grub-mkrescue not found (install grub-pc-bin / grub2-common)." >&2
    exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

mkdir -p "$workdir/boot/grub"

if [[ "$mode" == "fb" ]]; then
    # Framebuffer mode (Phase 2e): GRUB enables gfxterm + a VBE linear
    # framebuffer (640x480x32) and includes the framebuffer tag in the
    # multiboot2 info structure passed via %ebx. The kernel's boot.S saves it,
    # mb2.c parses it, and the software renderer draws the demo scene.
    cat > "$workdir/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

# Serial console for host-side logs (kernel curlee_putc -> COM1).
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

# Enable a linear framebuffer so the multiboot2 framebuffer tag is present.
set gfxmode=640x480x32
set gfxpayload=keep
insmod all_video
insmod gfxterm
terminal_output gfxterm

menuentry "JOE (Curlee bare-metal kernel)" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF
else
    # TEXT mode (default): the kernel writes the VGA text buffer at 0xB8000.
    # No gfxterm — keeps Phase 1 / VirtualBox behavior (0xB8000 stays live).
    cat > "$workdir/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

# Serial console for host-side logs (kernel curlee_putc -> COM1).
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

# IMPORTANT: stay in TEXT mode (no gfxterm). The kernel writes the VGA text
# buffer at 0xB8000; GRUB's gfxterm would switch to graphics where the text
# plane is not displayed. Text mode keeps 0xB8000 live (Phase 1 path).
menuentry "JOE (Curlee bare-metal kernel)" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF
fi

cp "$kernel_elf" "$workdir/boot/kernel.elf"

grub-mkrescue -o "$out_iso" "$workdir" 2>/dev/null
echo "ISO written: $out_iso"
