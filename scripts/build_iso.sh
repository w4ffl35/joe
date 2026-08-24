#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# build_iso.sh — package a GRUB-bootable ISO for VirtualBox.
#
# Usage:
#   bash scripts/build_iso.sh <kernel.elf> <out.iso>
#
# The kernel ELF must contain a multiboot2 header (kernel/boot.S provides it
# via the GRUB-path link in the Makefile). grub-mkrescue wraps the ELF and a
# grub.cfg into an El Torito ISO that VirtualBox (and qemu -cdrom) can boot:
# GRUB loads the ELF, finds the multiboot2 header, and enters the kernel's
# 64-bit entry, which calls curlee_main.
#
# Display strategy:
#   - GRUB's gfxterm loads a VBE linear framebuffer mode (640x480x32) before
#     booting the kernel. GRUB does this in real mode, so no long-mode BIOS
#     transition is needed in the kernel.
#   - The kernel then writes 0x00RRGGBB pixels as Phys<U32> to the linear
#     framebuffer at 0xFD00_0000 (the VBE base VirtualBox/QEMU expose for
#     this mode), which is presented natively on the VM display.
#   - Serial (COM1) remains enabled for host-side verification.
#
# Requires: grub-mkrescue (from grub-pc-bin / grub2-common on Debian/Ubuntu).
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <kernel.elf> <out.iso>" >&2
    exit 2
fi
kernel_elf="$1"
out_iso="$2"

if ! command -v grub-mkrescue >/dev/null 2>&1; then
    echo "error: grub-mkrescue not found (install grub-pc-bin / grub2-common)." >&2
    exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

mkdir -p "$workdir/boot/grub"

cat > "$workdir/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

# Serial console for host-side logs (kernel curlee_putc -> COM1).
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

# IMPORTANT: stay in TEXT mode (no gfxterm). The kernel writes the VGA text
# buffer at 0xB8000. GRUB's gfxterm switches to a graphics mode where the
# text plane is not displayed, and the kernel cannot reliably re-enter text
# mode or obtain the real framebuffer address (multiboot2 info is not passed
# to a 64-bit ELF entry). Text mode keeps 0xB8000 live.
menuentry "JOE (Curlee bare-metal kernel)" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF

cp "$kernel_elf" "$workdir/boot/kernel.elf"

grub-mkrescue -o "$out_iso" "$workdir" 2>/dev/null
echo "ISO written: $out_iso"
