/* SPDX-License-Identifier: GPL-3.0
 *
 * linker-grub.ld — JOE GRUB-path (ISO) linker script: 32-bit ELF output.
 *
 * Phase 2e-2: the multiboot2 spec guarantees the boot info pointer in %ebx
 * only for 32-bit protected-mode entries, and GNU ld refuses to link
 * ELFCLASS32 + ELFCLASS64 objects into one image. The GRUB-path kernel
 * (kernel-grub.elf) is therefore a FULLY 32-bit ELF: boot.S is assembled
 * with `as --32` and the codegen C + drivers are compiled with `-m32`,
 * linked here with `ld -m elf_i386`.
 *
 * Layout mirrors the Curlee runtime linker.ld (same symbols the stub needs):
 *   - .text/.rodata/.data/.bss at the multiboot base 0x100000.
 *   - .bss is followed by a 16 KiB stack region whose top is exported as
 *     `__stack_top` (boot.S sets %esp to it).
 *   - `__bss_start` / `__bss_end` bracket the zero-initialized data that
 *     boot.S clears before calling curlee_main.
 *
 * The multiboot2 header must be within the first 32768 bytes of the file and
 * 8-byte aligned; placing .multiboot2 first at 0x100000 satisfies this.
 */

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        KEEP(*(.multiboot2))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(16);
    }

    /* Static stack: 16 KiB of reserved zero-initialized storage. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        __stack_bottom = .;
        . += 16K;
        __stack_top = .;
    }
}
