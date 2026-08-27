// SPDX-License-Identifier: GPL-3.0
//
// boot.S — GRUB (ISO path) 32-bit multiboot2 entry stub.
//
// Phase 2e-2: the multiboot2 spec guarantees the boot info pointer in %ebx
// ONLY for 32-bit protected-mode entries. GRUB's x86-64 ELF entry (ELFCLASS64,
// long mode) leaves %ebx undefined and does not reliably load .data — verified
// empirically (docs/phase2e-architecture.md §7). GNU ld also refuses to link
// ELFCLASS32 + ELFCLASS64 objects in one image.
//
// Therefore the GRUB-path kernel (kernel-grub.elf) is a FULLY 32-bit ELF:
//   - boot.S is assembled with `as --32` (ELFCLASS32 object).
//   - The codegen C + drivers are compiled with `-m32` and linked with
//     `ld -m elf_i386` (Makefile kernel-grub.elf recipe).
//   - GRUB's multiboot2 loader enters a 32-bit ELF in 32-bit protected mode
//     with %ebx = trusted info pointer (the documented case), flat segments
//     (CS/DS base 0, limit 0xFFFFFFFF) and paging disabled.
//
// This stub captures %ebx into the global mb2_info_addr (which
// kernel/mb2.curlee's mb2_parse reads directly — the extern static declared
// there, gh issue #31; the old kernel/mb2_state.c shim + mb2_info_addr_get
// are deleted — to find the framebuffer tag — gh issue #15), zeroes .bss,
// sets the stack, calls curlee_main, and halts. No GDT or page tables are
// needed: the
// multiboot2 handoff already provides flat 32-bit segments with paging off,
// so the kernel addresses physical memory directly (same model as the VGA
// text buffer at 0xB8000).
//
// The 64-bit QEMU/PVH path (kernel.elf via crt0.S) is completely unchanged;
// this file is used ONLY by the ISO path (make iso / make iso-fb).

.set MB2_MAGIC,   0xE85250D6   /* multiboot2 */
.set MB2_ARCH,    0            /* i386 (protected mode) */
.set MB2_HEADER_SIZE, 16
.set MB2_TAG_END, 8

.section .multiboot2, "a"
.balign 8
multiboot2_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long multiboot2_header_end - multiboot2_header
    .long -(MB2_MAGIC + MB2_ARCH + (multiboot2_header_end - multiboot2_header))
    /* Framebuffer request tag (type 5): ask GRUB for a 640x480x32 linear
       framebuffer. The bootloader sets this mode before entry (independent
       of gfxterm), so the multiboot2 info structure's framebuffer tag
       (type 8) reports a real LFB that mb2.curlee's mb2_parse finds
       (bpp == 32 — gh issue #15). */
    .word 5          /* type: framebuffer request */
    .word 0          /* flags */
    .long 20         /* size: width + height + depth */
    .long 640        /* width */
    .long 480        /* height */
    .long 32         /* depth (bpp) */
    /* Header tags must be 8-byte aligned; the 20-byte fb-request tag ends at
       offset 36, so pad 4 bytes so the end tag starts at offset 40. */
    .balign 8
    /* End tag (must be the last tag). */
    .word 0
    .word 0
    .long MB2_TAG_END
multiboot2_header_end:

/* 32-bit entry — GRUB enters here in protected mode with %ebx = info ptr. */
.section .text
.global _start
.type _start, @function
.code32
_start:
    /* 0. Zero .bss. __bss_start/__bss_end are absolute symbols from
          linker-grub.ld; the size is computed at link time. This MUST run
          before the %ebx store below: the codegen's extern-static global
          mb2_info_addr (kernel/mb2.curlee, gh issue #31) is
          zero-initialized, so gcc places it in .bss — a store before this
          loop would be wiped by it. */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx              /* ecx = bss size in bytes */
    xorl %eax, %eax              /* fill byte: 0 */
1:  testl %ecx, %ecx
    jz 2f
    movb %al, (%edi)
    incl %edi
    decl %ecx
    jmp 1b
2:
    /* 1. Save the multiboot2 info pointer (%ebx, spec-guaranteed for 32-bit
          entries) into the global mb2_info_addr. Explicitly zero the high
          dword: never trust memory initializers before we have written them
          ourselves (GRUB's ELF loading of .data is not assumed). The symbol
          is DEFINED by the Curlee codegen (kernel/mb2.curlee's
          `extern static mb2_info_addr: Int = 0;` — a plain int64_t global
          under this exact name, no curlee_ mangle, curlee #297); the old
          weak .data definition in kernel/mb2_state.c is deleted (gh issue
          #31), so this object only REFERENCES the symbol. */
    movl %ebx, mb2_info_addr
    movl $0, mb2_info_addr + 4

    /* 2. Stack pointer to the top of the reserved stack region. */
    movl $__stack_top, %esp
    xorl %ebp, %ebp              /* clear frame pointer */

    /* 3. Call the verified kernel entry point (codegen emits curlee_main;
          compiled -m32 for this path, so the call is a plain 32-bit call). */
    call curlee_main

    /* 4. Deterministic halt. */
    cli
3:  hlt
    jmp 3b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits
