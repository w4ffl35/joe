// SPDX-License-Identifier: GPL-3.0
//
// vbe_state.c — the one remaining C residual of the former kernel/vbe.c
// (deleted in gh issue #11).
//
// The whole Bochs VBE probe — port I/O at 0x1CE/0x1CF, mode programming,
// readback validation, the LFB sanity gate — was ported to Curlee
// (kernel/vbe.curlee, `vbe_probe`). The framebuffer globals it fills
// (fb_addr/fb_pitch/fb_width/fb_height) are C-visible .data state owned by
// kernel/fb.c (still C — the blitter), and Curlee has no global variables
// and no way to address C globals, so the four stores stay here as a raw
// state shim — the second pillar of the C boundary policy
// (docs/c-boundary-policy.md §1: "No logic in C — only I/O touches and raw
// memory moves."). No logic, no port I/O: just four assignments, the same
// class as vga_text_clear.c's raw memory move.
//
// Freestanding: no libc.

// Framebuffer state owned by kernel/fb.c (non-static .data globals so the
// GRUB multiboot2 parser kernel/mb2.c and this shim can fill them).
extern unsigned int fb_addr;
extern unsigned int fb_pitch;
extern unsigned int fb_width;
extern unsigned int fb_height;

// Fill the framebuffer globals from a validated 32bpp linear-framebuffer
// probe. Called by kernel/vbe.curlee's `vbe_probe` only after its full
// validation gate (VBE ID present + mode readback matches + LFB address
// range) passes. The Int parameters arrive as int64_t (Curlee codegen);
// the globals are u32, and the caller's values are always in range
// (0xFD000000, 2560, 640, 480).
void vbe_state_set(long long addr, long long pitch,
                   long long width, long long height)
{
    fb_addr = (unsigned int)addr;
    fb_pitch = (unsigned int)pitch;
    fb_width = (unsigned int)width;
    fb_height = (unsigned int)height;
}
