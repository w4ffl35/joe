// SPDX-License-Identifier: GPL-3.0
//
// boot.S — GRUB (VirtualBox ISO path) entry stub.
//
// GRUB's multiboot2 loader supports x86-64 ELF kernels: when the ELF entry
// is a 64-bit entry (as here), GRUB sets up long mode itself and enters the
// kernel in 64-bit long mode with identity paging — the same state the qemu
// -kernel (PVH) path provides. So this stub mirrors curlee's runtime/crt0.S:
// zero .bss, set the stack, call `curlee_main`, halt.
//
// This file is assembled with `as --64` and linked with curlee's linker.ld
// (which KEEP(.multiboot2) and provides __bss_start/__bss_end/__stack_top).
// Used ONLY by the VirtualBox ISO path; the primary qemu path uses crt0.S.

.set MB2_MAGIC,   0xE85250D6   /* multiboot2 */
.set MB2_ARCH,    0            /* i386 (protected mode) */
.set MB2_HEADER_SIZE, 16
.set MB2_TAG_END, 8

.section .multiboot2, "a"
.align 8
multiboot2_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long multiboot2_header_end - multiboot2_header
    .long -(MB2_MAGIC + MB2_ARCH + (multiboot2_header_end - multiboot2_header))
    /* End tag (must be the last tag). */
    .word 0
    .word 0
    .long MB2_TAG_END
multiboot2_header_end:

/* 64-bit entry — GRUB enters here in long mode. */
.section .text
.global _start
.type _start, @function
_start:
    /* 1. Zero .bss. __bss_start/__bss_end are absolute symbols from
          linker.ld; the size is computed at link time. */
    movabs $__bss_start, %rax
    movabs $__bss_end, %rcx
    sub %rax, %rcx              /* rcx = bss size in bytes */
    xor %al, %al                /* fill byte: 0 */
1:  test %rcx, %rcx
    jz 2f
    movb %al, (%rax)
    inc %rax
    dec %rcx
    jmp 1b
2:
    /* 2. Stack pointer to the top of the reserved stack region. */
    movabs $__stack_top, %rsp
    xor %rbp, %rbp              /* clear frame pointer */

    /* 3. Call the verified kernel entry point (codegen emits curlee_main). */
    call curlee_main

    /* 4. Deterministic halt. */
    cli
3:  hlt
    jmp 3b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits
