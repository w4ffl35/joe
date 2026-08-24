# SPDX-License-Identifier: GPL-3.0
#
# JOE — Phase 1 bare-metal kernel build.
#
# Primary path (QEMU):  curlee build --link -> build/kernel.elf
#   - Uses Curlee's stock crt0.S + linker.ld (multiboot2 + PVH, 64-bit entry).
#   - qemu-system-x86_64 -kernel build/kernel.elf boots it directly.
#
# Secondary path (VirtualBox):  make iso -> build/joeos.iso
#   - GRUB-bootable ISO via grub-mkrescue wrapping kernel.elf, using our
#     boot.S (multiboot2 32-bit entry + long-mode trampoline) so GRUB can
#     enter the 64-bit curlee_main.

SHELL := /usr/bin/env bash

CURLEE ?= $(shell bash scripts/find-curlee.sh)

BUILD_DIR     := build
KERNEL_ELF    := $(BUILD_DIR)/kernel.elf
ISO           := $(BUILD_DIR)/joeos.iso
KERNEL_SRC    := kernel/kernel.curlee
PACK_SRC      := kernel/pack.curlee
BOOT_ASM      := kernel/boot.S
DRIVER_C      := kernel/putc_driver.c
VGA_SETUP_C   := kernel/vga_setup.c
FB_C          := kernel/fb.c
# Curlee runtime (crt0.S, linker.ld, rt.c) lives at the source root, not under
# build/. Prefer CURLEE_ROOT (repo root); otherwise derive it from the curlee
# binary location (curlee is at <root>/build/<preset>/curlee).
ifdef CURLEE_ROOT
CURLEE_RT := $(CURLEE_ROOT)/runtime
else
CURLEE_RT := $(shell dirname $$(dirname $(CURLEE)))/../runtime
endif

CC := cc
AS := as
LD := ld

.PHONY: all kernel check pack-run iso qemu run verify clean

all: kernel

# ---------------------------------------------------------------------------
# Kernel ELF (QEMU path) — uses Curlee's own crt0.S/linker.ld.
# ---------------------------------------------------------------------------
kernel: $(KERNEL_ELF)

# Primary QEMU kernel. We build it like the smoke path (codegen -> compile ->
# assemble -> link) so the serial driver (putc_driver.c) and the VGA text-mode
# setup (vga_setup.c) are always linked in, not just in the smoke test.
$(KERNEL_ELF): $(KERNEL_SRC) $(DRIVER_C) $(VGA_SETUP_C) $(FB_C)
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel.c $(KERNEL_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/putc_driver.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel.o $(BUILD_DIR)/putc_driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $@
	@echo "Built $@"
	@echo "Entry:"; objdump -f $@ | grep 'start address'

# ---------------------------------------------------------------------------
# Verification gates
# ---------------------------------------------------------------------------
check: $(KERNEL_SRC) $(PACK_SRC)
	$(CURLEE) check $(KERNEL_SRC)
	$(CURLEE) check $(PACK_SRC)
	@echo "curlee check: OK (all modules verified)"

# Pure packer is VM-runnable; assert the deterministic cell math.
pack-run: $(PACK_SRC)
	$(CURLEE) run $(PACK_SRC)

# ---------------------------------------------------------------------------
# GRUB ISO (VirtualBox path)
# ---------------------------------------------------------------------------
iso: $(ISO)

# Build the multiboot2-bootable kernel ELF used by GRUB: emit freestanding C
# from the kernel, compile it + the serial driver + our boot.S, link with
# Curlee's linker script (which provides __bss_start/__bss_end/__stack_top and
# KEEP(.multiboot2)).
$(BUILD_DIR)/kernel-grub.elf: $(KERNEL_SRC) $(BOOT_ASM) $(DRIVER_C) $(VGA_SETUP_C) $(FB_C)
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel.c $(KERNEL_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel-grub.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/driver.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	# boot.S: single x86-64 object with the multiboot2 header + 32-bit entry +
	# long-mode transition + 64-bit _start (assembled with --64, one entry).
	$(AS) --64 $(BOOT_ASM) -o $(BUILD_DIR)/boot.o
	$(LD) -nostdlib -static -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel-grub.o $(BUILD_DIR)/driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/rt.o $(BUILD_DIR)/boot.o \
	  -o $@
	@echo "Built $@ (GRUB multiboot2)"

$(ISO): $(BUILD_DIR)/kernel-grub.elf
	bash scripts/build_iso.sh $(BUILD_DIR)/kernel-grub.elf $(ISO)
	@echo "Built $@"

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
# QEMU boot with serial captured to build/serial.log (observable boot check).
qemu: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -display none -no-reboot

# QEMU with serial on the terminal — the "console screen" for headless dev.
# The kernel's output (curlee_putc -> COM1) prints live to your terminal,
# exactly what the machine "says" on boot. Press Ctrl-A X to quit.
qemu-serial: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial stdio -display none -no-reboot

# QEMU with a real GUI window (like VirtualBox). Use this if you're on a
# desktop session with a display server; the VGA text "Hello World from JOE!"
# renders in the window. Falls back to VNC if no display is available.
qemu-display: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -no-reboot

# QEMU with a viewable display (VNC on 127.0.0.1:5901) — connect with a VNC
# client if you want to see the rendered VGA text.
qemu-gui: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -vnc 127.0.0.1:1 -no-reboot

# Build the kernel with the serial driver override (not the weak no-op) and
# boot it, asserting the serial log contains the expected message. This is
# the dynamic acceptance gate: it proves the whole pipeline (verify -> codegen
# -> compile -> assemble -> link -> PVH boot -> curlee_main -> display + serial).
qemu-smoke: check
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel-smoke.c $(KERNEL_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel-smoke.c -o $(BUILD_DIR)/kernel-smoke.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/putc_driver.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel-smoke.o $(BUILD_DIR)/putc_driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $(BUILD_DIR)/kernel-smoke.elf
	rm -f $(BUILD_DIR)/serial.log
	@timeout 20 qemu-system-x86_64 -display none -no-reboot \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -kernel $(BUILD_DIR)/kernel-smoke.elf || true
	@grep -q 'Hello World from JOE' $(BUILD_DIR)/serial.log \
	  && echo "PASS: qemu boot -> serial output: $$(cat $(BUILD_DIR)/serial.log)" \
	  || (echo "FAIL: expected message not in serial log"; exit 1)

# Boot the GRUB ISO under qemu (sanity check for the VirtualBox path).
qemu-iso: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot d \
	  -serial file:$(BUILD_DIR)/serial-iso.log \
	  -display none -no-reboot

run: qemu

# ---------------------------------------------------------------------------
# Verify (all acceptance gates)
# ---------------------------------------------------------------------------
verify: check pack-run kernel
	@echo "=== Verification gates ==="
	@test -s $(KERNEL_ELF) || (echo "FAIL: kernel.elf missing"; exit 1)
	@objdump -f $(KERNEL_ELF) | grep -q 'start address 0x' && echo "PASS: ELF entry set"
	@nm $(KERNEL_ELF) | grep -q ' _start$$' && echo "PASS: _start present"
	@nm $(KERNEL_ELF) | grep -q ' curlee_main$$' && echo "PASS: curlee_main present"
	@readelf -S $(KERNEL_ELF) | grep -q '\.note\.Xen' && echo "PASS: PVH note (qemu -kernel)"
	@echo "All verification gates passed."

clean:
	rm -rf $(BUILD_DIR)
