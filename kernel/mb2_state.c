// SPDX-License-Identifier: GPL-3.0
//
// mb2_state.c — the C residual of the former kernel/mb2.c (deleted in gh
// issue #15).
//
// The whole multiboot2 info structure tag walk — the runtime-address reads,
// the 8-byte tag alignment math, the framebuffer-tag field extraction, the
// trust gates — was ported to Curlee (kernel/mb2.curlee, `mb2_parse`). What
// remains here is ONLY the raw-state half that Curlee cannot express (the
// C-visible .data globals), per docs/c-boundary-policy.md §1 ("No logic in
// C — only I/O touches and raw memory moves"), the vbe_state.c precedent
// (gh issue #11):
//
//   - mb2_info_addr_get(): reads the .data global `mb2_info_addr` that
//     kernel/boot.S fills from %ebx on the GRUB path. The weak default (0,
//     owned here) keeps the PVH path linking (no boot.S); boot.S's strong
//     .data definition overrides it on the GRUB/ISO path.
//   - mb2_state_set(): the four framebuffer-globals stores (fb_addr/pitch/
//     width/height, C-visible .data owned by kernel/fb.c) — the exact
//     vbe_state_set shape.
//
// The former phys_read_u8()/phys_read_u32() definitions were DELETED: the
// rebuilt toolchain made them Curlee COMPILER BUILTINS (inline volatile
// loads in the freestanding codegen — kernel/mb2.curlee calls them directly
// inside `unsafe` with `cap phys.mem`), so the C symbols are dead.
//
// No logic: no tag walking, no alignment math, no field extraction — just
// two getters and the four global stores.
//
// Freestanding: no libc (freestanding <stdint.h> only).

#include <stdint.h>

// The multiboot2 info pointer captured by kernel/boot.S (%ebx on entry).
// The WEAK default (0) lives here (moved from kernel/fb.c in gh issue #13 —
// this module's mb2_info_addr_get is its only reader, so it is the genuine
// owner): on the PVH path (qemu -kernel, crt0.S — no boot.S) it provides the
// symbol as 0; on the GRUB/ISO path boot.S's strong .data definition
// overrides it (GNU weak/strong interposition).
unsigned long long mb2_info_addr __attribute__((weak)) = 0;

// Framebuffer state owned by kernel/fb.c (non-static .data globals so this
// shim can fill them — the blitter reads them).
extern unsigned int fb_addr;
extern unsigned int fb_pitch;
extern unsigned int fb_width;
extern unsigned int fb_height;

// The multiboot2 info address, as a Curlee Int (int64_t). 0 when no boot
// stub captured a pointer (PVH path). Called by kernel/mb2.curlee's
// mb2_parse before any tag walk.
long long mb2_info_addr_get(void)
{
    return (long long)mb2_info_addr;
}

// Fill the framebuffer globals from a usable 32bpp framebuffer tag. Called
// by kernel/mb2.curlee's mb2_parse only after its full validation gate
// (non-zero info addr, < 4 GiB, total_size bounds, 32bpp, non-zero fb addr)
// passes. The U32 parameters arrive as uint32_t (Curlee codegen); the
// globals are unsigned int, and the caller's values always fit.
void mb2_state_set(uint32_t addr, uint32_t pitch, uint32_t width,
                   uint32_t height)
{
    fb_addr = addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
}
