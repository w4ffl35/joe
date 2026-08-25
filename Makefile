# SPDX-License-Identifier: GPL-3.0
#
# JOE — Phase 2 agentic framebuffer OS kernel build.
#
# Primary path (QEMU):  build-kernel.sh merge -> curlee build --link -> build/kernel.elf
#   - scripts/build-kernel.sh concatenates the pure renderer modules
#     (canvas/glyphs/assets) + kernel.curlee into a single translation unit
#     (the freestanding codegen crashes on imports — see
#     docs/phase2-architecture.md §1), then Curlee's stock crt0.S + linker.ld
#     produce a multiboot2 + PVH 64-bit kernel ELF.
#   - qemu-system-x86_64 -kernel build/kernel.elf boots it directly.
#
# Secondary path (VirtualBox):  make iso -> build/joeos.iso
#   - GRUB-bootable ISO via grub-mkrescue wrapping kernel.elf, using our
#     boot.S (multiboot2 32-bit entry + long-mode trampoline).
#
# Verification gates:
#   make check      -> curlee check every pure module + the merged kernel
#   make canvas-run -> curlee run kernel/canvas_test.curlee (asserts pure math)
#   make qemu-smoke -> boot kernel.elf, assert serial log contains the message

SHELL := /usr/bin/env bash

CURLEE ?= $(shell bash scripts/find-curlee.sh)

BUILD_DIR     := build
KERNEL_ELF    := $(BUILD_DIR)/kernel.elf
ISO           := $(BUILD_DIR)/joeos.iso
MERGED_SRC    := $(BUILD_DIR)/kernel-merged.curlee
KERNEL_SRC    := kernel/kernel.curlee
PACK_SRC      := kernel/pack.curlee
CANVAS_SRC    := kernel/canvas.curlee
GLYPHS_SRC    := kernel/glyphs.curlee
ASSETS_SRC    := kernel/assets.curlee
CANVAS_TEST   := kernel/canvas_test.curlee
BOOT_ASM      := kernel/boot.S
DRIVER_C      := kernel/putc_driver.c
VGA_SETUP_C   := kernel/vga_setup.c
FB_C          := kernel/fb.c
MB2_C         := kernel/mb2.c
VBE_C         := kernel/vbe.c
NET_C         := kernel/virtio_net.c
NET_H         := kernel/net.h
LIBGCC32_C    := kernel/libgcc32.c
LINKER_GRUB   := scripts/linker-grub.ld
MERGE_SCRIPT  := scripts/build-kernel.sh
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

.PHONY: all kernel check pack-run canvas-run iso iso-fb qemu run verify clean \
        qemu-smoke qemu-fb-smoke qemu-loop-smoke qemu-pvh-fb-smoke qemu-net-smoke

all: kernel

# ---------------------------------------------------------------------------
# Kernel ELF (QEMU path) — merge + codegen + compile + link.
# ---------------------------------------------------------------------------
kernel: $(KERNEL_ELF)

# Merge the pure modules + kernel.curlee into a single-TU file, then verify +
# codegen it. The merged file depends on the modules so any change re-merges.
$(MERGED_SRC): $(KERNEL_SRC) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC) $(MERGE_SCRIPT)
	@mkdir -p $(BUILD_DIR)
	bash $(MERGE_SCRIPT) $@

$(KERNEL_ELF): $(MERGED_SRC) $(DRIVER_C) $(VGA_SETUP_C) $(FB_C) $(MB2_C) $(VBE_C) $(NET_C) $(NET_H)
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel.c $(MERGED_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/putc_driver.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	# PVH path (qemu -kernel): compile-time-empty frame ring/asset region
	# (JOE_PVH_BOOT) so the image stays within QEMU's PVH LOAD budget (a large
	# BSS silently breaks the PVH entry — see kernel/fb.c header). The GRUB/ISO
	# path (kernel-grub.elf) compiles WITHOUT this macro and gets the full
	# ring/asset region, which is where the framebuffer flip actually runs.
	$(CC) -DJOE_PVH_BOOT -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(MB2_C) -o $(BUILD_DIR)/mb2.o
	# Phase 2f: Bochs VBE probe (kernel/vbe.c) — PVH-only. The GRUB/ISO path
	# (kernel-grub.elf) does NOT link it (dead code there: the multiboot2 tag
	# always wins; see the kernel-grub.elf rule below).
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VBE_C) -o $(BUILD_DIR)/vbe.o
	# Phase 2d-1: VirtIO-net driver (kernel/virtio_net.c). PVH path — option 1
	# ACTIVE (JOE_PVH_BOOT): 1-byte ring/buffer stubs, every extern returns 0,
	# net_probe() is a safe no-op (the PVH machine has no legacy PCI config
	# space — see the file header). The GRUB/ISO path compiles WITHOUT the
	# macro and gets the full option-2 rings (see kernel-grub.elf below).
	$(CC) -DJOE_PVH_BOOT -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(NET_C) -o $(BUILD_DIR)/virtio_net.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel.o $(BUILD_DIR)/putc_driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/mb2.o $(BUILD_DIR)/vbe.o $(BUILD_DIR)/virtio_net.o $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $@
	@echo "Built $@"
	@echo "Entry:"; objdump -f $@ | grep 'start address'

# ---------------------------------------------------------------------------
# Verification gates
# ---------------------------------------------------------------------------
# kernel.curlee is only valid when merged (it calls helpers from the modules),
# so `check` verifies the modules standalone + the merged kernel.
check: $(PACK_SRC) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC) $(MERGED_SRC)
	$(CURLEE) check $(PACK_SRC)
	$(CURLEE) check $(CANVAS_SRC)
	$(CURLEE) check $(GLYPHS_SRC)
	$(CURLEE) check $(ASSETS_SRC)
	$(CURLEE) check $(MERGED_SRC)
	@echo "curlee check: OK (all modules + merged kernel verified)"

# Pure packer is VM-runnable; assert the deterministic cell math.
pack-run: $(PACK_SRC)
	$(CURLEE) run $(PACK_SRC)

# Pure renderer math is VM-runnable; assert color/geometry/glyph/asset math.
canvas-run: $(CANVAS_TEST) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC)
	$(CURLEE) run $(CANVAS_TEST)

# ---------------------------------------------------------------------------
# GRUB ISO (VirtualBox path)
# ---------------------------------------------------------------------------
iso: $(ISO)

# Build the multiboot2-bootable kernel ELF used by GRUB: emit freestanding C
# from the merged kernel, compile it + the serial driver + boot.S, link with
# scripts/linker-grub.ld (which provides __bss_start/__bss_end/__stack_top and
# KEEP(.multiboot2)).
#
# Phase 2e-2: this path is a FULLY 32-BIT ELF (kernel-grub.elf). The multiboot2
# spec guarantees the boot info pointer in %ebx ONLY for 32-bit protected-mode
# entries, and GNU ld refuses to link ELFCLASS32 + ELFCLASS64 objects into one
# image. So every object here is -m32, boot.S is assembled with `as --32`, and
# the link uses `ld -m elf_i386`. GRUB's multiboot2 loader enters this kernel
# in 32-bit protected mode with %ebx = the trusted info pointer; boot.S stores
# it into mb2_info_addr and calls curlee_main (compiled -m32, so the 64-bit
# Int math is implemented by libgcc32.o's __muldi3/__divdi3/... helpers).
#
# The 64-bit PVH/QEMU path (kernel.elf via crt0.S) is completely unchanged.
$(BUILD_DIR)/kernel-grub.elf: $(MERGED_SRC) $(BOOT_ASM) $(DRIVER_C) $(VGA_SETUP_C) $(FB_C) $(MB2_C) $(LIBGCC32_C) $(NET_C) $(NET_H) $(LINKER_GRUB)
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel.c $(MERGED_SRC)
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel-grub.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/driver.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(MB2_C) -o $(BUILD_DIR)/mb2.o
	# Phase 2d-1: VirtIO-net driver — GRUB/ISO path compiles WITHOUT JOE_PVH_BOOT,
	# so option 2 is ACTIVE: full rings (2 RX x 2048 + 2 TX x 2048, depth 256)
	# and the NIC runs (this is where qemu-net-smoke boots; see the target).
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(NET_C) -o $(BUILD_DIR)/virtio_net.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(LIBGCC32_C) -o $(BUILD_DIR)/libgcc32.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	# boot.S: ELFCLASS32 object with the multiboot2 header + 32-bit protected-mode
	# entry that captures %ebx and calls curlee_main directly (no GDT/page tables:
	# the multiboot2 handoff already provides flat 32-bit segments, paging off).
	$(AS) --32 $(BOOT_ASM) -o $(BUILD_DIR)/boot.o
	$(LD) -m elf_i386 -nostdlib -static -T $(LINKER_GRUB) \
	  $(BUILD_DIR)/kernel-grub.o $(BUILD_DIR)/driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/mb2.o $(BUILD_DIR)/virtio_net.o $(BUILD_DIR)/libgcc32.o $(BUILD_DIR)/rt.o $(BUILD_DIR)/boot.o \
	  -o $@
	@echo "Built $@ (GRUB multiboot2, 32-bit entry)"

$(ISO): $(BUILD_DIR)/kernel-grub.elf
	bash scripts/build_iso.sh $(BUILD_DIR)/kernel-grub.elf $(ISO) text
	@echo "Built $@ (text mode)"

# Framebuffer-mode ISO (Phase 2e): GRUB gfxterm + linear framebuffer, so the
# multiboot2 framebuffer tag is present and the renderer activates.
$(BUILD_DIR)/joeos-fb.iso: $(BUILD_DIR)/kernel-grub.elf
	bash scripts/build_iso.sh $(BUILD_DIR)/kernel-grub.elf $@ fb
	@echo "Built $@ (framebuffer mode)"

.PHONY: iso-fb
iso-fb: $(BUILD_DIR)/joeos-fb.iso

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
# QEMU boot with serial captured to build/serial.log (observable boot check).
qemu: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -display none -no-reboot

# QEMU with serial on the terminal — the "console screen" for headless dev.
qemu-serial: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial stdio -display none -no-reboot

# QEMU with a real GUI window (like VirtualBox). Use this if you're on a
# desktop session with a display server; the VGA text renders in the window.
qemu-display: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -no-reboot

# QEMU with a viewable display (VNC on 127.0.0.1:5901).
qemu-gui: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -vnc 127.0.0.1:1 -no-reboot

# Build the kernel with the serial driver override and boot it, asserting the
# serial log contains the expected message. This is the dynamic acceptance
# gate: it proves the whole pipeline (merge -> verify -> codegen -> compile ->
# assemble -> link -> PVH boot -> curlee_main -> display + serial).
# The PVH smoke kernel: built like kernel.elf (JOE_PVH_BOOT, crt0.S +
# linker.ld, vbe.c linked) but with its own objects so the smoke gates never
# clobber the dev-loop build. Shared by qemu-smoke and qemu-pvh-fb-smoke.
$(BUILD_DIR)/kernel-smoke.elf: check
	@mkdir -p $(BUILD_DIR)
	$(CURLEE) build --target freestanding-c -o $(BUILD_DIR)/kernel-smoke.c $(MERGED_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel-smoke.c -o $(BUILD_DIR)/kernel-smoke.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(DRIVER_C) -o $(BUILD_DIR)/putc_driver.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VGA_SETUP_C) -o $(BUILD_DIR)/vga_setup.o
	$(CC) -DJOE_PVH_BOOT -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(FB_C) -o $(BUILD_DIR)/fb.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(MB2_C) -o $(BUILD_DIR)/mb2.o
	# Phase 2f: vbe.c is PVH-only (see the kernel.elf rule).
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(VBE_C) -o $(BUILD_DIR)/vbe.o
	# Phase 2d-1: VirtIO-net driver, PVH option-1 stubs (JOE_PVH_BOOT) — the
	# NIC is compiled IN so the no-NIC-safe path is exercised on the existing
	# smoke gates (net_probe finds nothing, all externs return 0).
	$(CC) -DJOE_PVH_BOOT -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(NET_C) -o $(BUILD_DIR)/virtio_net.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel-smoke.o $(BUILD_DIR)/putc_driver.o $(BUILD_DIR)/vga_setup.o $(BUILD_DIR)/fb.o \
	  $(BUILD_DIR)/mb2.o $(BUILD_DIR)/vbe.o $(BUILD_DIR)/virtio_net.o $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $@

# Boot the PVH kernel (qemu -kernel) and assert the serial log contains the
# expected message. Proves the whole pipeline (merge -> verify -> codegen ->
# compile -> assemble -> link -> PVH boot -> curlee_main -> display + serial).
qemu-smoke: $(BUILD_DIR)/kernel-smoke.elf
	rm -f $(BUILD_DIR)/serial.log
	@timeout 20 qemu-system-x86_64 -display none -no-reboot \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -kernel $(BUILD_DIR)/kernel-smoke.elf || true
	@grep -q 'Hello World from JOE' $(BUILD_DIR)/serial.log \
	  && echo "PASS: qemu boot -> serial output: $$(cat $(BUILD_DIR)/serial.log)" \
	  || (echo "FAIL: expected message not in serial log"; exit 1)

# Phase 2f acceptance gate: boot the PVH kernel (qemu -kernel, kernel-smoke.elf
# built exactly like kernel.elf with JOE_PVH_BOOT + vbe.c) with a std VGA
# device and assert the serial log contains "FB: 1". This proves the Bochs VBE
# probe validated a linear framebuffer on the PVH path (no multiboot2 info, no
# ISO), fb_ready() returned 1, and the single-frame demo render ran before
# halt. Mirror of the qemu-smoke pattern: timeout, -display none, -serial
# file:, grep.
qemu-pvh-fb-smoke: $(BUILD_DIR)/kernel-smoke.elf
	rm -f $(BUILD_DIR)/serial-pvh-fb.log
	@timeout 20 qemu-system-x86_64 -display none -no-reboot \
	  -vga std -serial file:$(BUILD_DIR)/serial-pvh-fb.log \
	  -kernel $(BUILD_DIR)/kernel-smoke.elf || true
	@grep -q 'FB: 1' $(BUILD_DIR)/serial-pvh-fb.log \
	  && echo "PASS: PVH path framebuffer active (fb_ready=1 via VBE probe) -> serial: $$(cat $(BUILD_DIR)/serial-pvh-fb.log)" \
	  || (echo "FAIL: FB: 1 marker not in serial log (PVH VBE probe / draw target broken)"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-pvh-fb.log)"; exit 1)

# Phase 2e gate: boot the FB-mode GRUB ISO under QEMU with a linear
# framebuffer and assert the serial log contains the "FB:" marker — proving
# the multiboot2 framebuffer plumbing works end-to-end (boot.S saved %ebx,
# mb2.c parsed the framebuffer tag, fb_ready() returned 1, render_frame ran).
# Phase 2c: also assert "RING: 1" — the frame ring activated and fb_present()
# performed a real back-buffer flip during the loop.
qemu-fb-smoke: $(BUILD_DIR)/joeos-fb.iso
	rm -f $(BUILD_DIR)/serial-fb.log
	@timeout 25 qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-fb.iso -boot d -no-reboot \
	  -vga std -serial file:$(BUILD_DIR)/serial-fb.log \
	  -display none || true
	@grep -q 'FB:' $(BUILD_DIR)/serial-fb.log \
	  && echo "PASS: framebuffer active (fb_ready=1) -> serial: $$(cat $(BUILD_DIR)/serial-fb.log)" \
	  || (echo "FAIL: FB: marker not in serial log (framebuffer plumbing broken)"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-fb.log)"; exit 1)
	@grep -q 'RING: 1' $(BUILD_DIR)/serial-fb.log \
	  && echo "PASS: frame ring active (fb_present flip ran)" \
	  || (echo "FAIL: RING: 1 marker not in serial log (frame ring did not flip)"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-fb.log)"; exit 1)

# Phase 2b gate: boot the FB-mode ISO and assert the 60 FPS loop actually
# ran the FULL ordered sequence — the serial log must contain, IN ORDER,
# FR:0, FR:1, FR:2, FR:3 (the deterministic loop renders 4 frames), then
# RING: 1 (Phase 2c: the frame ring flipped — fb_present ran), then FB: 1,
# then the Phase 1 serial + halt. Grepping the exact ordered sequence (not
# just individual markers) catches a loop that skips or reorders a frame
# mid-way, deterministically and within the timeout.
qemu-loop-smoke: $(BUILD_DIR)/joeos-fb.iso
	rm -f $(BUILD_DIR)/serial-loop.log
	@timeout 25 qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-fb.iso -boot d -no-reboot \
	  -vga std -serial file:$(BUILD_DIR)/serial-loop.log \
	  -display none || true
	@grep -Pzo 'FR:0\nFR:1\nFR:2\nFR:3\nRING: 1\nFB: 1\nHello World from JOE!\n' \
	    $(BUILD_DIR)/serial-loop.log > /dev/null \
	  && echo "PASS: 60 FPS loop ran the full ordered sequence (FR:0..FR:3, RING: 1, FB: 1, Hello World from JOE!) -> serial: $$(cat $(BUILD_DIR)/serial-loop.log)" \
	  || (echo "FAIL: serial log does not contain the exact ordered loop sequence"; \
	      echo "expected: FR:0 FR:1 FR:2 FR:3 RING: 1 FB: 1 Hello World from JOE!"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-loop.log)"; exit 1)

# Phase 2d-1 acceptance gate: boot the GRUB ISO with a VirtIO-net NIC attached
# and assert the serial log contains, IN ORDER, NET: 1 (PCI found), NET: 2
# (device ready / rings up), NET: 3 (link up), and RX: <len> (the first RX
# frame).
#
# Frame-injection mechanism (LOCKED in issue #5, adjusted for QEMU 10):
# the plan's slirp ARP-answer (guest ARPs the gateway 10.0.2.2, slirp
# auto-replies) does NOT fire on QEMU 10.0.11 user-net in practice (verified:
# no RX with -netdev user; slirp only answers once it has a route/DHCP entry
# for the guest). The DETERMINISTIC replacement keeps the same guest-ARP
# mechanism but uses a second QEMU instance connected via a socket netdev:
# the SENDER instance's broadcast ARP request is delivered by the socket
# backend to the RECEIVER instance's NIC, which reports it via RX: <len>.
# No host-side injection, no live network, fully deterministic — and it
# exercises the exact RX path 2d-2 will consume.
#
# This gate boots the GRUB/ISO path (kernel-grub.elf, no JOE_PVH_BOOT) with
# the full option-2 rings, because the QEMU `-kernel` PVH machine (xenpvh)
# exposes NO legacy PCI config space (docs/phase2f-report.md §4) — the NIC is
# only reachable where SeaBIOS runs (the ISO boot). The gate uses a dedicated
# ISO (joeos-net.iso) so the existing qemu-fb-smoke / qemu-loop-smoke gates
# are untouched.
$(BUILD_DIR)/joeos-net.iso: $(BUILD_DIR)/kernel-grub.elf
	bash scripts/build_iso.sh $(BUILD_DIR)/kernel-grub.elf $@ text

qemu-net-smoke: $(BUILD_DIR)/joeos-net.iso
	rm -f $(BUILD_DIR)/serial-net.log $(BUILD_DIR)/serial-net-sender.log
	# Free the socket port from any stale listener (e.g. an interrupted run).
	@(command -v fuser >/dev/null 2>&1 && fuser -k 11000/tcp 2>/dev/null) || true
	@sleep 1
	# RECEIVER: the gate's subject — boots first, listens on the socket.
	@qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-net.iso -boot d -no-reboot \
	  -netdev socket,id=n0,listen=127.0.0.1:11000 \
	  -device virtio-net-pci,disable-modern=on,netdev=n0 \
	  -serial file:$(BUILD_DIR)/serial-net.log \
	  -display none > $(BUILD_DIR)/net-recv.err 2>&1 &
	@echo $$! > $(BUILD_DIR)/net-recv.pid
	@sleep 2
	# SENDER: boots second, connects to the receiver, broadcasts the ARP.
	@timeout 20 qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-net.iso -boot d -no-reboot \
	  -netdev socket,id=n0,connect=127.0.0.1:11000 \
	  -device virtio-net-pci,disable-modern=on,netdev=n0 \
	  -serial file:$(BUILD_DIR)/serial-net-sender.log \
	  -display none > $(BUILD_DIR)/net-send.err 2>&1 || true
	@kill $$(cat $(BUILD_DIR)/net-recv.pid) 2>/dev/null || true
	@rm -f $(BUILD_DIR)/net-recv.pid
	@grep -Pzo 'NET: 1\nNET: 2\nNET: 3\n' $(BUILD_DIR)/serial-net.log > /dev/null \
	  && echo "PASS: virtio-net bring-up (NET: 1 -> NET: 2 -> NET: 3) -> serial: $$(cat $(BUILD_DIR)/serial-net.log)" \
	  || (echo "FAIL: NET: 1..3 markers not in order in serial log"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-net.log)"; exit 1)
	@grep -q 'RX: ' $(BUILD_DIR)/serial-net.log \
	  && echo "PASS: RX frame received (socket netdev delivery) -> serial: $$(cat $(BUILD_DIR)/serial-net.log)" \
	  || (echo "FAIL: RX: marker not in serial log (no frame received)"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-net.log)"; exit 1)

# Boot the GRUB ISO under qemu (sanity check for the VirtualBox path).
qemu-iso: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot d \
	  -serial file:$(BUILD_DIR)/serial-iso.log \
	  -display none -no-reboot

run: qemu

# ---------------------------------------------------------------------------
# Verify (all acceptance gates)
# ---------------------------------------------------------------------------
verify: check pack-run canvas-run kernel
	@echo "=== Verification gates ==="
	@test -s $(KERNEL_ELF) || (echo "FAIL: kernel.elf missing"; exit 1)
	@objdump -f $(KERNEL_ELF) | grep -q 'start address 0x' && echo "PASS: ELF entry set"
	@nm $(KERNEL_ELF) | grep -q ' _start$$' && echo "PASS: _start present"
	@nm $(KERNEL_ELF) | grep -q ' curlee_main$$' && echo "PASS: curlee_main present"
	@readelf -S $(KERNEL_ELF) | grep -q '\.note\.Xen' && echo "PASS: PVH note (qemu -kernel)"
	@echo "All verification gates passed."

clean:
	rm -rf $(BUILD_DIR)
