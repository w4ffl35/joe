// SPDX-License-Identifier: GPL-3.0
//
// mb2_state.c — the C residual of the former kernel/mb2.c (deleted in gh
// issue #15), reduced to its minimum in gh issue #20.
//
// The whole multiboot2 info structure tag walk — the runtime-address reads,
// the 8-byte tag alignment math, the framebuffer-tag field extraction, the
// trust gates — is genuine Curlee (kernel/mb2.curlee, `mb2_parse`). The
// framebuffer state it fills is Curlee statics in kernel/fb.curlee (filled
// through its fb_state_set function — the former mb2_state_set setter was
// DELETED in gh issue #20, along with the vbe_state.c twin).
//
// What remains here, and ONLY why (per docs/c-boundary-policy.md §1 — "No
// logic in C — only I/O touches and raw memory moves"): the single raw-state
// half that Curlee CANNOT express — the multiboot2 info pointer itself.
//
//   - mb2_info_addr: the .data global that kernel/boot.S fills from %ebx on
//     the GRUB path (32-bit assembly, BEFORE curlee_main runs). The WEAK
//     default (0, owned here) keeps the PVH path linking (no boot.S);
//     boot.S's strong .data definition overrides it on the GRUB/ISO path
//     (GNU weak/strong interposition).
//   - mb2_info_addr_get(): the Curlee window into that global (0 on the PVH
//     path). Curlee statics are file-local in the codegen (static C
//     symbols), so boot.S cannot write one — a C-level linkage requirement,
//     the same class as virtio_net.c's sfence and the PVH-conditional
//     buffers.
//
// The former phys_read_u8()/phys_read_u32() definitions were DELETED long
// ago: the rebuilt toolchain made them Curlee COMPILER BUILTINS (inline
// volatile loads in the freestanding codegen — kernel/mb2.curlee calls them
// directly inside `unsafe` with `cap phys.mem`).
//
// No logic: no tag walking, no alignment math, no field extraction — one
// weak .data global and one getter.
//
// Freestanding: no libc (freestanding <stdint.h> only).

#include <stdint.h>

// The multiboot2 info pointer captured by kernel/boot.S (%ebx on entry).
// The WEAK default (0) lives here (this module's mb2_info_addr_get is its
// only reader, so it is the genuine owner): on the PVH path (qemu -kernel,
// crt0.S — no boot.S) it provides the symbol as 0; on the GRUB/ISO path
// boot.S's strong .data definition overrides it (GNU weak/strong
// interposition).
unsigned long long mb2_info_addr __attribute__((weak)) = 0;

// The multiboot2 info address, as a Curlee Int (int64_t). 0 when no boot
// stub captured a pointer (PVH path). Called by kernel/mb2.curlee's
// mb2_parse before any tag walk.
long long mb2_info_addr_get(void)
{
    return (long long)mb2_info_addr;
}
