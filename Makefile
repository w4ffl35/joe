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
FB_SRC        := kernel/fb.curlee
JSON_SRC      := kernel/json.curlee
SERIAL_SRC    := kernel/serial.curlee
VGA_SETUP_SRC := kernel/vga_setup.curlee
VBE_SRC       := kernel/vbe.curlee
VIRTIO_NET_SRC := kernel/virtio_net.curlee
E1000_SRC     := kernel/e1000.curlee
NET_STACK_SRC := kernel/net_stack.curlee
NET_GLUE_SRC  := kernel/net_glue.curlee
# Pure virtio-blk modules (raw sector-read driver foundation). All are
# standalone-checkable pure Curlee (no Phys, no extern) — see the per-module
# slice commits. Wired into `check` only; they join the merged kernel TU
# when the real driver lands.
VIRTIO_BLK_SRC := kernel/virtio_blk_helpers.curlee kernel/virtio_blk_layout.curlee \
                  kernel/virtio_blk_queue.curlee kernel/virtio_blk_requests.curlee \
                  kernel/virtio_blk_bounds.curlee kernel/virtio_blk_reqbuf.curlee \
                  kernel/virtio_blk_alloc.curlee
CANVAS_TEST   := kernel/canvas_test.curlee
JSON_TEST     := kernel/json_test.curlee
NET_STACK_TEST := kernel/net_stack_test.curlee
BOOT_ASM      := kernel/boot.S
MB2_SRC       := kernel/mb2.curlee
LINKER_GRUB   := scripts/linker-grub.ld
MERGE_SCRIPT  := scripts/build-kernel.sh

# Per-build-target static array sizing (gh issue #296): the SAME merged
# kernel.curlee source declares the large static buffers (fb.curlee's asset
# region + frame ring, virtio_net.curlee's ring/buffer memory) with sizes
# that are constant expressions over these build-time constants, injected
# with `curlee build --define NAME=VALUE` (no C shim, no conditional
# compilation). kernel/fb.c + kernel/virtio_net.c are DELETED.
#   PVH build (qemu -kernel) — PVH_DEFINES: the buffers stub to 1 element
#   (JOE_PVH_BOOT=1) so the image stays within QEMU's PVH LOAD budget
#   (docs/phase2c-report.md §4.3). kernel.elf + kernel-smoke.elf.
#   GRUB build (kernel-grub.elf / ISO) — GRUB_DEFINES: full option-2 sizes,
#   where the framebuffer flip / the NIC actually runs.
#   check — CHECK_DEFINES: the full-size (GRUB) geometry (the harder verify
#   case; the canvas_test/VM gates assert the pure constants on both sides).
PVH_DEFINES := --define JOE_PVH_BOOT=1 \
  --define ASSET_REGION_W=1 --define ASSET_REGION_H=1 \
  --define FRAME_RING_SLOTS=1 --define FRAME_RING_MAX_W=1 --define FRAME_RING_MAX_H=1 \
  --define NET_QMEM_PAGES=1 --define NET_RX_BUFS=1 --define NET_TX_BUFS=1 --define NET_BUF_BYTES=1
GRUB_DEFINES := --define JOE_PVH_BOOT=0 \
  --define ASSET_REGION_W=128 --define ASSET_REGION_H=128 \
  --define FRAME_RING_SLOTS=2 --define FRAME_RING_MAX_W=640 --define FRAME_RING_MAX_H=480 \
  --define NET_QMEM_PAGES=5 --define NET_RX_BUFS=2 --define NET_TX_BUFS=2 --define NET_BUF_BYTES=2048
CHECK_DEFINES := $(GRUB_DEFINES)
# Curlee runtime (crt0.S, linker.ld, rt.c, libgcc32_helpers.c) lives at the
# source root, not under build/. Prefer CURLEE_ROOT (repo root); otherwise
# derive it from the curlee binary location (curlee is at
# <root>/build/<preset>/curlee).
ifdef CURLEE_ROOT
CURLEE_RT := $(CURLEE_ROOT)/runtime
else
CURLEE_RT := $(shell dirname $$(dirname $(CURLEE)))/../runtime
endif

# 32-bit GCC ABI helpers (__muldi3/__udivdi3/__umoddi3/__udivmoddi4/
# __divdi3/__moddi3/__negdi2/__ashldi3/__lshrdi3/__ashrdi3/__cmpdi2) for the
# GRUB/ISO path (kernel-grub.elf). curlee #288 bundled these into the
# toolchain runtime (runtime/libgcc32_helpers.c) so downstream projects never
# ship their own copy — gh issue #21 deleted kernel/libgcc32.c and this build
# consumes the toolchain-bundled file instead (same symbols, one source of
# truth). Only linked into kernel-grub.elf: the 64-bit PVH path (kernel.elf)
# has native int64_t arithmetic and never needs them.
LIBGCC32_HELPERS_C := $(CURLEE_RT)/libgcc32_helpers.c

CC := cc
AS := as
LD := ld

.PHONY: all kernel check pack-run canvas-run json-run json-codegen-run net-stack-run net-stack-codegen-run mb2-codegen-run iso iso-fb qemu run verify clean \
        qemu-smoke qemu-fb-smoke qemu-loop-smoke qemu-pvh-fb-smoke qemu-net-smoke qemu-llm-smoke qemu-e1000-smoke \
        c-boundary

all: kernel

# ---------------------------------------------------------------------------
# Kernel ELF (QEMU path) — merge + codegen + compile + link.
# ---------------------------------------------------------------------------
kernel: $(KERNEL_ELF)

# Merge the pure modules + kernel.curlee into a single-TU file, then verify +
# codegen it. The merged file depends on the modules so any change re-merges.
$(MERGED_SRC): $(KERNEL_SRC) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC) $(FB_SRC) $(JSON_SRC) $(SERIAL_SRC) $(VGA_SETUP_SRC) $(VBE_SRC) $(VIRTIO_NET_SRC) $(E1000_SRC) $(NET_STACK_SRC) $(NET_GLUE_SRC) $(MB2_SRC) $(MERGE_SCRIPT)
	@mkdir -p $(BUILD_DIR)
	bash $(MERGE_SCRIPT) $@

$(KERNEL_ELF): $(MERGED_SRC)
	@mkdir -p $(BUILD_DIR)
	# PVH path (qemu -kernel): the large Curlee buffers (asset region, frame
	# ring, net ring/buffer memory) are sized to 1-element stubs by the PVH
	# --define set (JOE_PVH_BOOT=1) so the image stays within QEMU's PVH LOAD
	# budget (docs/phase2c-report.md §4.3 — a large BSS silently breaks the
	# PVH entry). The GRUB/ISO path (kernel-grub.elf below) uses the GRUB
	# define set and gets the full ring/asset region, where the framebuffer
	# flip actually runs (gh issue #296).
	$(CURLEE) build --target freestanding-c $(PVH_DEFINES) -o $(BUILD_DIR)/kernel.c $(MERGED_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel.o
	# gh issue #15 + #20 + #31: the multiboot2 tag walk is Curlee
	# (kernel/mb2.curlee, merged into kernel.c above) — the C residual is
	# GONE. The boot-handoff info pointer is now a Curlee extern static
	# (`extern static mb2_info_addr: Int = 0;` in mb2.curlee, curlee #297):
	# the codegen emits it as a plain `int64_t mb2_info_addr = 0;` global
	# (external linkage, verbatim name) inside kernel.c, so kernel/mb2_state.c
	# (weak .data global + mb2_info_addr_get) is DELETED and nothing C links
	# for it. On this PVH path no boot stub writes the symbol, so it stays at
	# its 0 default and mb2_parse no-ops; the GRUB path (kernel-grub.elf)
	# links boot.S, whose `movl %ebx, mb2_info_addr` stores into the same
	# codegen global. The framebuffer state the parse fills is Curlee statics
	# in fb.curlee (fb_state_set); the runtime-address phys_read_u8/u32 reads
	# are Curlee compiler builtins (curlee #279).
	# Phase 2f: Bochs VBE probe — ported to Curlee in gh issue #11
	# (kernel/vbe.curlee, merged into kernel.c above). No C residual remains
	# (gh issue #20): the framebuffer state it fills is Curlee statics in
	# fb.curlee, written through fb_state_set — the former vbe_state.c shim
	# (`vbe_state_set`) is deleted and nothing from it links here.
	# Phase 2d-1 (gh issue #296): the VirtIO-net driver is GENUINE Curlee —
	# kernel/virtio_net.c is DELETED. The ring/buffer memory is Curlee statics
	# sized per build via --define (PVH: 1-element stubs / option 1, GRUB:
	# full option-2 rings), the base-address getters are Curlee addr_of reads,
	# and the ring-publication fence lives in the Curlee runtime (curlee_sfence,
	# rt.c — the old virtio_net_sfence extern is gone). Nothing C links here.
	# Phase 2d-2 (gh issue #12): TCP/IP stack — the ARP/IPv4/TCP logic is
	# net_stack.curlee + the kernel.curlee glue, and since gh issue #20 the
	# mutable one-shot state + 256-byte response store are Curlee statics in
	# net_glue.curlee (the kernel/net_stack.c raw-state shim is DELETED —
	# no C object links here).
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel.o \
	  $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $@
	@echo "Built $@"
	@echo "Entry:"; objdump -f $@ | grep 'start address'

# ---------------------------------------------------------------------------
# Verification gates
# ---------------------------------------------------------------------------
# kernel.curlee is only valid when merged (it calls helpers from the modules),
# so `check` verifies the modules standalone + the merged kernel.
check: $(PACK_SRC) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC) $(FB_SRC) $(JSON_SRC) $(SERIAL_SRC) $(VGA_SETUP_SRC) $(VIRTIO_NET_SRC) $(E1000_SRC) $(NET_STACK_SRC) $(VIRTIO_BLK_SRC) $(MERGED_SRC)
	$(CURLEE) check $(PACK_SRC)
	$(CURLEE) check $(CANVAS_SRC)
	$(CURLEE) check $(GLYPHS_SRC)
	$(CURLEE) check $(ASSETS_SRC)
	# gh issue #296: fb.curlee's large buffers are sized by --define build
	# constants, so the standalone check passes the full-size (GRUB) define
	# set (the harder verify case; the PVH stub case is exercised by the PVH
	# kernel build below).
	$(CURLEE) check $(CHECK_DEFINES) $(FB_SRC)
	$(CURLEE) check $(JSON_SRC)
	$(CURLEE) check $(SERIAL_SRC)
	$(CURLEE) check $(VGA_SETUP_SRC)
	# gh issue #14 + #296: the VirtIO-net driver is genuine Curlee; its
	# ring/buffer statics are sized by --define (checked with the full GRUB
	# define set here) and its only extern is the runtime curlee_sfence.
	$(CURLEE) check $(CHECK_DEFINES) $(VIRTIO_NET_SRC)
	# Workstream C (fabrication-fix plan): the e1000 detect + reset driver is
	# genuine Curlee (PCI config via 0xCF8/0xCFC + MMIO via the runtime-address
	# phys_read_u32/phys_write_u32 builtins — no Phys<T> literals, no C shim).
	# Standalone-checkable (no cross-module calls); re-verified in the merged TU.
	$(CURLEE) check $(E1000_SRC)
	# The pure protocol core stays VM-checkable standalone (extern-free).
	$(CURLEE) check $(NET_STACK_SRC)
	# The pure virtio-blk foundation modules (slice-built, standalone pure).
	# curlee check takes ONE file, so loop the 7 modules individually.
	$(CURLEE) check kernel/virtio_blk_helpers.curlee
	$(CURLEE) check kernel/virtio_blk_layout.curlee
	$(CURLEE) check kernel/virtio_blk_queue.curlee
	$(CURLEE) check kernel/virtio_blk_requests.curlee
	$(CURLEE) check kernel/virtio_blk_bounds.curlee
	$(CURLEE) check kernel/virtio_blk_reqbuf.curlee
	$(CURLEE) check kernel/virtio_blk_alloc.curlee
	# gh issue #20: kernel/vbe.curlee and kernel/mb2.curlee are NOT checked
	# standalone anymore — they call fb.curlee's fb_state_set (the shared
	# framebuffer-state setter), so they verify through the MERGED TU below
	# (the net_glue.curlee precedent). The merged TU needs the full define
	# set for the same reason as the standalone fb/net checks.
	$(CURLEE) check $(CHECK_DEFINES) $(MERGED_SRC)
	@echo "curlee check: OK (all modules + merged kernel verified)"

# Pure packer is VM-runnable; assert the deterministic cell math.
pack-run: $(PACK_SRC)
	$(CURLEE) run $(PACK_SRC)

# C boundary policy gate (docs/c-boundary-policy.md): no logic in C — only
# I/O touches and raw memory moves. Fails if a kernel/*.c exceeds the line
# cap or contains pure-data red flags (switch tables, large static consts).
c-boundary:
	bash scripts/check-c-boundary.sh

# Pure renderer math is VM-runnable; assert color/geometry/glyph/asset math.
canvas-run: $(CANVAS_TEST) $(CANVAS_SRC) $(GLYPHS_SRC) $(ASSETS_SRC)
	$(CURLEE) run $(CANVAS_TEST)

# Phase 2d-3: pure JSON/tool-call parser is VM-runnable; assert the scanner
# phases the VM emitter can execute (see kernel/json_test.curlee's scope note).
json-run: $(JSON_TEST) $(JSON_SRC)
	$(CURLEE) run $(JSON_TEST)

# Phase 2d-3: the FULL locked 36-byte envelope (and the [12,34] / [0,-1,2]
# regression payloads) through the freestanding codegen path — the VM cannot
# feed 36 bytes (emitter size limit), but `curlee build` codegens to host-
# runnable C with no such limit. This is the authoritative happy-path gate
# (acceptance criterion 1): asserts err=0, tool="frame_tick", args=[0,1,2].
json-codegen-run:
	bash scripts/run-json-codegen.sh

# Phase 2d-2 (gh issue #12): the pure TCP/IP protocol core (net_stack.curlee)
# is VM-runnable; assert the checksums, wire bytes and the HTTP response state
# machine against the ground truth captured from the former C implementation.
net-stack-run: $(NET_STACK_TEST) $(NET_STACK_SRC)
	$(CURLEE) run --fuel 1500000 $(NET_STACK_TEST)

# Phase 2d-2 (gh issue #12): the HOST-SIDE wire proof of the one-shot glue —
# codegens net_stack.curlee + net_glue.curlee and drives net_connect /
# net_send / net_stack_poll / net_response_len / net_response_byte against a
# scripted NIC (replayed SYN-ACK + the split stub response), asserting the
# staged SYN/ACK/REQ frames byte-for-byte against the C ground truth. This is
# the json-codegen-run precedent: identical wire behavior with no NIC and no
# live gate (qemu-llm-smoke needs host port 8080 free).
net-stack-codegen-run:
	bash scripts/run-net-stack-codegen.sh

# gh issue #15: the HOST-SIDE proof of the Curlee multiboot2 tag walk
# (kernel/mb2.curlee, ported from the deleted kernel/mb2.c). The GRUB boot
# gates (qemu-fb-smoke / qemu-loop-smoke) prove the live path; this harness
# proves the SAME parser contract deterministically with no QEMU: it codegens
# mb2.curlee and drives curlee mb2_parse against scripted physical memory
# across 12 scenarios (happy path, every trust gate — incl. the negative
# info-addr case the C original's unsigned >= 4 GiB gate rejects — malformed
# tags, the 8-byte alignment math), asserting BOTH the return value AND the
# extracted framebuffer state — identical to the C ground truth (the
# json-codegen-run / net-stack-codegen-run precedent).
mb2-codegen-run:
	bash scripts/run-mb2-codegen.sh

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
# Int math is implemented by the 32-bit GCC ABI helpers — since curlee #288 /
# gh issue #21 these come from the TOOLCHAIN-BUNDLED runtime
# (libgcc32_helpers.c, $(CURLEE_RT)); the kernel-local kernel/libgcc32.c is
# deleted).
#
# The 64-bit PVH/QEMU path (kernel.elf via crt0.S) is completely unchanged.
$(BUILD_DIR)/kernel-grub.elf: $(MERGED_SRC) $(BOOT_ASM) $(LIBGCC32_HELPERS_C) $(NET_H) $(LINKER_GRUB)
	@mkdir -p $(BUILD_DIR)
	# GRUB/ISO path (gh issue #296): the GRUB --define set (JOE_PVH_BOOT=0)
	# sizes the Curlee buffers to the FULL geometry — 128x128 asset region,
	# 2x640x480 frame ring, 5-page net qmem + 2x2048 data buffers — which is
	# where the framebuffer flip and the NIC actually run. kernel/fb.c +
	# kernel/virtio_net.c are DELETED; nothing C links for them.
	$(CURLEE) build --target freestanding-c $(GRUB_DEFINES) -o $(BUILD_DIR)/kernel.c $(MERGED_SRC)
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel.c -o $(BUILD_DIR)/kernel-grub.o
	# gh issue #15 + #20 + #31: the multiboot2 tag walk is Curlee
	# (kernel/mb2.curlee, merged into kernel.c) — the raw-state shim is GONE.
	# The boot-handoff info pointer is a Curlee extern static in mb2.curlee
	# (`extern static mb2_info_addr: Int = 0;`, curlee #297): the codegen in
	# kernel-grub.o defines the plain `int64_t mb2_info_addr` global, and
	# boot.S's `movl %ebx, mb2_info_addr` / `movl $0, mb2_info_addr + 4`
	# stores into it AFTER zeroing .bss (the zero-initialized global lands in
	# .bss — a store before the zeroing would be wiped). This is the LIVE
	# path: mb2_parse finds the framebuffer tag (the GRUB acceptance path).
	# The framebuffer state the parse fills is Curlee statics in fb.curlee
	# (fb_state_set — the former mb2_state_set setter is deleted, gh issue
	# #20); the phys_read_u8/u32 reads are Curlee compiler builtins.
	# Phase 2d-1 (gh issue #296): the VirtIO-net driver is GENUINE Curlee with
	# the full option-2 rings (the GRUB define set) — this is where
	# qemu-net-smoke boots. The former ring/buffer shim (kernel/virtio_net.c)
	# is deleted; the ring memory + data buffers are Curlee statics and the
	# getters are Curlee addr_of reads, so no virtio C object links here.
	# 32-bit GCC ABI helpers — curlee #288: bundled in the toolchain runtime
	# (libgcc32_helpers.c). gh issue #21 deleted kernel/libgcc32.c; the link
	# below resolves __muldi3/__udivdi3/... from this toolchain-owned object.
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(LIBGCC32_HELPERS_C) -o $(BUILD_DIR)/libgcc32.o
	$(CC) -m32 -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	# boot.S: ELFCLASS32 object with the multiboot2 header + 32-bit protected-mode
	# entry that captures %ebx and calls curlee_main directly (no GDT/page tables:
	# the multiboot2 handoff already provides flat 32-bit segments, paging off).
	$(AS) --32 $(BOOT_ASM) -o $(BUILD_DIR)/boot.o
	$(LD) -m elf_i386 -nostdlib -static -T $(LINKER_GRUB) \
	  $(BUILD_DIR)/kernel-grub.o \
	  $(BUILD_DIR)/libgcc32.o $(BUILD_DIR)/rt.o $(BUILD_DIR)/boot.o \
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
# linker.ld, vbe.curlee merged) but with its own objects so the smoke gates
# never clobber the dev-loop build. Shared by qemu-smoke and
# qemu-pvh-fb-smoke.
$(BUILD_DIR)/kernel-smoke.elf: check
	@mkdir -p $(BUILD_DIR)
	# PVH smoke kernel (gh issue #296): built like kernel.elf with the PVH
	# --define set (JOE_PVH_BOOT=1) — the Curlee buffers stub to 1 element,
	# every getter returns 0, and the no-NIC-safe path is exercised on the
	# existing smoke gates (net_probe finds nothing). kernel/fb.c +
	# kernel/virtio_net.c are DELETED; nothing C links for them.
	$(CURLEE) build --target freestanding-c $(PVH_DEFINES) -o $(BUILD_DIR)/kernel-smoke.c $(MERGED_SRC)
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -Iruntime -c $(BUILD_DIR)/kernel-smoke.c -o $(BUILD_DIR)/kernel-smoke.o
	# gh issue #15 + #20 + #31: the multiboot2 tag walk is Curlee (merged
	# into kernel-smoke.c) and the boot-handoff info pointer is a Curlee
	# extern static in mb2.curlee (curlee #297) — no mb2 C shim links
	# anywhere (see the kernel.elf rule). The framebuffer state it fills is
	# Curlee statics in fb.curlee (fb_state_set); the phys_read_u8/u32 reads
	# are Curlee compiler builtins.
	# Phase 2d-2 (gh issue #12): TCP/IP stack — the one-shot state + response
	# store are Curlee statics in net_glue.curlee (gh issue #20), so nothing
	# C links here; on the no-NIC-safe smoke gates the glue's net_link_up()
	# gate keeps the whole round-trip a no-op and boot continues to VGA +
	# serial + halt.
	$(CC) -ffreestanding -fno-builtin -nostdlib -std=c11 -c $(CURLEE_RT)/rt.c -o $(BUILD_DIR)/rt.o
	$(CC) -ffreestanding -fno-builtin -nostdlib -c $(CURLEE_RT)/crt0.S -o $(BUILD_DIR)/crt0.o
	$(LD) -nostdlib -T $(CURLEE_RT)/linker.ld \
	  $(BUILD_DIR)/kernel-smoke.o \
	  $(BUILD_DIR)/rt.o $(BUILD_DIR)/crt0.o \
	  -o $@

# Boot the PVH kernel (qemu -kernel) and assert the serial log contains the
# expected message. Proves the whole pipeline (merge -> verify -> codegen ->
# compile -> assemble -> link -> PVH boot -> curlee_main -> display + serial).
qemu-smoke: $(BUILD_DIR)/kernel-smoke.elf
	rm -f $(BUILD_DIR)/serial.log
	@timeout 20 qemu-system-x86_64 -display none -no-reboot -net none \
	  -serial file:$(BUILD_DIR)/serial.log \
	  -kernel $(BUILD_DIR)/kernel-smoke.elf || true
	@grep -q 'Hello World from JOE' $(BUILD_DIR)/serial.log \
	  && echo "PASS: qemu boot -> serial output: $$(cat $(BUILD_DIR)/serial.log)" \
	  || (echo "FAIL: expected message not in serial log"; exit 1)

# Phase 2f acceptance gate: boot the PVH kernel (qemu -kernel, kernel-smoke.elf
# built exactly like kernel.elf with JOE_PVH_BOOT + the Curlee vbe_probe from
# kernel/vbe.curlee) with a std VGA device and assert the serial log contains
# "FB: 1". This proves the Bochs VBE probe validated a linear framebuffer on
# the PVH path (no multiboot2 info, no ISO), fb_ready() returned 1, and the
# single-frame demo render ran before halt. Mirror of the qemu-smoke pattern:
# timeout, -display none, -serial file:, grep.
qemu-pvh-fb-smoke: $(BUILD_DIR)/kernel-smoke.elf
	rm -f $(BUILD_DIR)/serial-pvh-fb.log
	@timeout 20 qemu-system-x86_64 -display none -no-reboot -net none \
	  -vga std -serial file:$(BUILD_DIR)/serial-pvh-fb.log \
	  -kernel $(BUILD_DIR)/kernel-smoke.elf || true
	@grep -q 'FB: 1' $(BUILD_DIR)/serial-pvh-fb.log \
	  && echo "PASS: PVH path framebuffer active (fb_ready=1 via VBE probe) -> serial: $$(cat $(BUILD_DIR)/serial-pvh-fb.log)" \
	  || (echo "FAIL: FB: 1 marker not in serial log (PVH VBE probe / draw target broken)"; \
	      echo "serial log: $$(cat $(BUILD_DIR)/serial-pvh-fb.log)"; exit 1)

# Phase 2e gate: boot the FB-mode GRUB ISO under QEMU with a linear
# framebuffer and assert the serial log contains the "FB:" marker — proving
# the multiboot2 framebuffer plumbing works end-to-end (boot.S saved %ebx,
# mb2.curlee's mb2_parse walked the framebuffer tag, fb_ready() returned 1,
# render_frame ran — gh issue #15).
# Phase 2c: also assert "RING: 1" — the frame ring activated and fb_present()
# performed a real back-buffer flip during the loop.
qemu-fb-smoke: $(BUILD_DIR)/joeos-fb.iso
	rm -f $(BUILD_DIR)/serial-fb.log
	@timeout 25 qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-fb.iso -boot d -no-reboot -net none \
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
	@timeout 25 qemu-system-x86_64 -cdrom $(BUILD_DIR)/joeos-fb.iso -boot d -no-reboot -net none \
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
	# Phase 2d-2: the full ARP -> TCP -> HTTP round-trip needs slirp user-net
	# (the gateway 10.0.2.2 ARP answer + a real TCP peer), which this two-QEMU
	# socket gate does not provide — the full marker sequence (ARP: 1, TCP: 1,
	# SND: 36, RCV: 36) is asserted by the 2d-4 qemu-llm-smoke gate
	# (scripts/run-llm-smoke.sh, -netdev user against the host stub server).
	# This 2d-1 gate stays exactly as-is: NET: 1..3 + RX: <len>.

# ---------------------------------------------------------------------------
# Phase 2d-4 acceptance gate (GitHub issue #8): the end-to-end LLM round-trip.
# ---------------------------------------------------------------------------
# Boots the GRUB/ISO path (kernel-grub.elf, NIC compiled in — the PVH `-kernel`
# machine has no legacy PCI config space, so the NIC is only reachable where
# SeaBIOS runs) with slirp user-net, against the deterministic host stub
# (scripts/llm_stub_server.py on 127.0.0.1:8080, reachable from the guest at
# the 10.0.2.2 gateway alias — no hostfwd needed for kernel -> host), and
# asserts the FULL ordered marker sequence (docs/phase2d-wire.md §5):
#   NET: 1, NET: 2, NET: 3, ARP: 1, TCP: 1, SND: 36, RCV: 36,
#   JSON: 1, TOOL: 2, LLM: 1, Hello World from JOE!
# A "JSON: E<code>" marker fails the gate.
#
# The real-llama.cpp variant (documented, NOT CI-gated — nondeterministic
# model output would break the gate): run a llama server yourself and point
# the harness at it:
#   LLM_SERVER=skip make qemu-llm-smoke     # server already running on :8080
#   LLM_SERVER=llama-server make qemu-llm-smoke   # harness starts it
qemu-llm-smoke: $(BUILD_DIR)/joeos-net.iso
	bash scripts/run-llm-smoke.sh

# ---------------------------------------------------------------------------
# Workstream C acceptance gate (fabrication-fix plan): the REAL Intel e1000
# milestone — detect + reset, proven on real QEMU hardware with a real serial
# assertion. The gate is the checked-in, rerunnable artifact the harness
# (Workstream A) re-runs, making a fabricated "E1000: 1 / E1000: 2" report
# structurally impossible to pass.
#
# Boots the GRUB/ISO path (kernel-grub.elf — the PVH `-kernel` machine has no
# legacy PCI config space, docs/phase2f-report.md §4) with QEMU's real e1000
# NIC (`-device e1000,netdev=n0 -netdev user,id=n0`) and asserts the serial
# log contains, IN ORDER, "E1000: 1" (PCI detected) then "E1000: 2" (reset).
# The gate is timeout-bounded and fail-closed (missing/out-of-order markers
# -> FAIL, non-zero exit).
qemu-e1000-smoke: $(BUILD_DIR)/joeos-net.iso
	bash scripts/run-e1000-smoke.sh

# Boot the GRUB ISO under qemu (sanity check for the VirtualBox path).
qemu-iso: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot d \
	  -serial file:$(BUILD_DIR)/serial-iso.log \
	  -display none -no-reboot

run: qemu

# ---------------------------------------------------------------------------
# Verify (all acceptance gates)
# ---------------------------------------------------------------------------
verify: check pack-run canvas-run json-run json-codegen-run net-stack-run net-stack-codegen-run mb2-codegen-run c-boundary kernel
	@echo "=== Verification gates ==="
	@test -s $(KERNEL_ELF) || (echo "FAIL: kernel.elf missing"; exit 1)
	@objdump -f $(KERNEL_ELF) | grep -q 'start address 0x' && echo "PASS: ELF entry set"
	@nm $(KERNEL_ELF) | grep -q ' _start$$' && echo "PASS: _start present"
	@nm $(KERNEL_ELF) | grep -q ' curlee_main$$' && echo "PASS: curlee_main present"
	@readelf -S $(KERNEL_ELF) | grep -q '\.note\.Xen' && echo "PASS: PVH note (qemu -kernel)"
	# Phase 2d-2 (gh issue #12): the one-shot stack glue is now GENUINE Curlee
	# (codegen'd as curlee_net_connect & co — the C externs were removed) and
	# the mutable one-shot state + response store are Curlee statics in
	# net_glue.curlee (gh issue #20 — the kernel/net_stack.c raw-state shim is
	# deleted; the net_state_*/net_resp_*/net_poll_* C symbols are gone from
	# the ELF by design, their functions codegen as curlee_net_state_* & co).
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_connect$$' && echo "PASS: curlee_net_connect linked (Curlee glue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_send$$' && echo "PASS: curlee_net_send linked (Curlee glue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_stack_poll$$' && echo "PASS: curlee_net_stack_poll linked (Curlee glue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_response_len$$' && echo "PASS: curlee_net_response_len linked (Curlee glue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_response_byte$$' && echo "PASS: curlee_net_response_byte linked (Curlee glue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_state_get$$' && echo "PASS: curlee_net_state_get linked (Curlee state, gh issue #20)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_resp_store$$' && echo "PASS: curlee_net_resp_store linked (Curlee response store, gh issue #20)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_poll_fuel$$' && echo "PASS: curlee_net_poll_fuel linked (Curlee poll fuel, gh issue #20)"
	# gh issue #14 + #296: the VirtIO-net driver is now GENUINE Curlee (ported
	# from the 798-line kernel/virtio_net.c, which is DELETED) — the codegen
	# emits the driver surface as the static curlee_net_* symbols, the ring/
	# buffer memory + base getters are Curlee statics/addr_of reads (the
	# curlee_virtio_net_* symbols), and the ring-publication fence is the
	# runtime's curlee_sfence. Nothing C links for the driver on either build.
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_probe$$' && echo "PASS: curlee_net_probe linked (VirtIO-net driver, gh issue #14)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_init$$' && echo "PASS: curlee_net_init linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_rx_len$$' && echo "PASS: curlee_net_rx_len linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_rx_byte$$' && echo "PASS: curlee_net_rx_byte linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_tx_send$$' && echo "PASS: curlee_net_tx_send linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_rx_wait$$' && echo "PASS: curlee_net_rx_wait linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_net_arp_request_gateway$$' && echo "PASS: curlee_net_arp_request_gateway linked (VirtIO-net driver)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_virtio_net_rx_buf_base$$' && echo "PASS: curlee_virtio_net_rx_buf_base linked (Curlee RX buf getter, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_virtio_net_tx_buf_base$$' && echo "PASS: curlee_virtio_net_tx_buf_base linked (Curlee TX buf getter, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_virtio_net_rx_qmem_base$$' && echo "PASS: curlee_virtio_net_rx_qmem_base linked (Curlee RX qmem getter, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_virtio_net_pvh_build$$' && echo "PASS: curlee_virtio_net_pvh_build linked (Curlee build discriminator, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_sfence$$' && echo "PASS: curlee_sfence linked (runtime ring-publication fence, issue #296)"
	# Workstream C (fabrication-fix plan): the Intel e1000 detect + reset
	# driver is genuine Curlee (kernel/e1000.curlee) — the codegen emits the
	# driver surface as the static curlee_e1000_* symbols. No C shim links
	# (pure Curlee: PCI config via ports, MMIO via the phys builtins).
	@nm $(KERNEL_ELF) | grep -q ' curlee_e1000_probe$$' && echo "PASS: curlee_e1000_probe linked (e1000 PCI detect, Workstream C)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_e1000_reset$$' && echo "PASS: curlee_e1000_reset linked (e1000 MMIO reset, Workstream C)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_e1000_bringup$$' && echo "PASS: curlee_e1000_bringup linked (e1000 bring-up glue, Workstream C)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_serial_e1000_marker$$' && echo "PASS: curlee_serial_e1000_marker linked (E1000 serial markers, Workstream C)"
	# Phase 2d-4: the tool-queue producer API the LLM bridge drives
	# (fb_tool_enqueue(2, arg) — the 2d-3 contract, wired into the 2b ring).
	# gh issue #13: the blitter + event loop moved to Curlee (kernel/fb.curlee),
	# so the codegen emits the queue producer as the static curlee_fb_tool_enqueue
	# symbol (the C extern was removed); the ring flip + the runtime-address
	# memory moves are now ALL Curlee (the phys builtins inline in the codegen —
	# no C symbols), so the gates check the Curlee statics and the Curlee
	# addr_of getters (gh issue #296 — the C getters are gone with fb.c).
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_tool_enqueue$$' && echo "PASS: curlee_fb_tool_enqueue linked (Curlee tool queue)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_present$$' && echo "PASS: curlee_fb_present linked (Curlee blitter)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_ring_activate$$' && echo "PASS: curlee_fb_ring_activate linked (Curlee ring flip)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_ring_advance$$' && echo "PASS: curlee_fb_ring_advance linked (Curlee ring advance)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_frame_ring_slot_base$$' && echo "PASS: curlee_fb_frame_ring_slot_base linked (Curlee frame-ring base getter, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_asset_region_base_get$$' && echo "PASS: curlee_fb_asset_region_base_get linked (Curlee asset-region base getter, issue #296)"
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_pvh_build$$' && echo "PASS: curlee_fb_pvh_build linked (Curlee build discriminator, issue #296)"
	# Phase 2f (gh issue #11 + #20): the Curlee Bochs VBE probe
	# (kernel/vbe.curlee) fills the framebuffer state through fb.curlee's
	# fb_state_set — a genuine Curlee function (codegen'd as the static
	# curlee_fb_state_set symbol); the former vbe_state.c shim is deleted and
	# no C symbol links.
	@nm $(KERNEL_ELF) | grep -q ' curlee_fb_state_set$$' && echo "PASS: curlee_fb_state_set linked (Curlee framebuffer-state setter, gh issue #20)"
	# gh issue #15 + #20 + #31: the multiboot2 tag walk is now GENUINE Curlee
	# (ported from kernel/mb2.c) — the codegen emits it as curlee_mb2_parse,
	# and the boot-handoff info pointer is a Curlee extern static
	# (kernel/mb2.curlee's `extern static mb2_info_addr: Int = 0;`, curlee
	# #297): the codegen emits the plain `int64_t mb2_info_addr` global
	# (external linkage, verbatim name) in kernel.o — the kernel/mb2_state.c
	# shim (weak .data global + mb2_info_addr_get) is DELETED (gh issue #31),
	# so no C object provides the symbol. On this PVH build it stays at its 0
	# default (no boot.S); on the GRUB build boot.S stores %ebx into it. The
	# runtime-address reads (phys_read_u8/u32) are Curlee compiler builtins
	# (inline volatile loads — no C symbol).
	@nm $(KERNEL_ELF) | grep -q ' curlee_mb2_parse$$' && echo "PASS: curlee_mb2_parse linked (multiboot2 parser, gh issue #15)"
	@nm $(KERNEL_ELF) | grep -q ' mb2_info_addr$$' && echo "PASS: mb2_info_addr linked (Curlee extern static, gh issue #31)"
	@echo "All verification gates passed."

clean:
	rm -rf $(BUILD_DIR)
