// SPDX-License-Identifier: GPL-3.0
//
// vga_text_clear.c — the one remaining raw memory move of the former
// kernel/vga_setup.c (deleted in issue #10).
//
// The 24-write VGA CRT register sequence was ported to Curlee
// (kernel/vga_setup.curlee, `vga_text_setup`). The text-buffer clear could
// NOT be ported: Curlee's Phys<T> addresses must be compile-time literals
// (a computed address like 0xB8000 + i*2 is rejected — "expected ')' after
// phys address"), and the language has no runtime-address physical WRITE
// builtin (only the phys_read_u8/u16/u32/u64 READS of curlee issue #279).
// A bounded Curlee loop therefore cannot advance its write address; 2000
// unrolled literal writes are rejected by issue #10's scope and would blow
// QEMU's PVH LOAD budget. So the clear stays here as a raw memory move —
// the second pillar of the C boundary policy (docs/c-boundary-policy.md §1:
// "No logic in C — only I/O touches and raw memory moves.").
//
// Freestanding: volatile stores only, no libc.

// Blank the 0xB8000 VGA text buffer (80x25 cells, 2 bytes per cell) to
// blank cells: space (0x20) with attribute 0x0F (bright white on black).
void vga_clear_text_buffer(void)
{
    volatile unsigned short* text = (volatile unsigned short*)0xB8000;
    for (int i = 0; i < 80 * 25; ++i)
    {
        text[i] = 0x0F20; // space with attribute 0x0F
    }
}
