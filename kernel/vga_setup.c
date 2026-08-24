// SPDX-License-Identifier: GPL-3.0
//
// vga_setup.c — explicit VGA text-mode-3 programming for reliable text
// rendering under VirtualBox (and QEMU).
//
// Problem: after GRUB (multiboot2) hands off, the VGA is left in whatever
// mode/font state GRUB used. QEMU's VGA emulation happens to render standard
// PC-437 bytes at 0xB8000 correctly; VirtualBox's VGA emulation maps the
// text plane through a Unicode font that GRUB's state doesn't match, so
// "Hello" can appear as "Fcjjm" (a per-glyph offset) etc.
//
// Fix: program the VGA CRT Controller (MISC/SEQ/CRTC) for standard 80x25
// text mode 3 before writing to the 0xB8000 text buffer, and reset the
// sequencer font plane so VirtualBox uses its default PC-437 text font.
// This is the same sequence OS kernels run on every boot and is harmless on
// QEMU.
//
// Freestanding: only I/O-port outb, no libc.

#define VGA_MISC 0x3C2
#define VGA_SEQ  0x3C4
#define VGA_CRTC 0x3D4

static inline void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" ::"a"(value), "Nd"(port));
}

void vga_text_setup(void)
{
    // 1. MISC: color text mode, 28 MHz, page enabled.
    outb(VGA_MISC, 0x63);

    // 2. Sequencer: reset + enable 8x8/9x16 text font planes.
    outb(VGA_SEQ, 0x00); outb(VGA_SEQ + 1, 0x03); // synchronous reset
    outb(VGA_SEQ, 0x01); outb(VGA_SEQ + 1, 0x01); // clocking mode: 8 dots
    outb(VGA_SEQ, 0x02); outb(VGA_SEQ + 1, 0x0F); // plane mask: all 4
    outb(VGA_SEQ, 0x03); outb(VGA_SEQ + 1, 0x00); // char map select A
    outb(VGA_SEQ, 0x04); outb(VGA_SEQ + 1, 0x0E); // memory mode: even/odd, no odd/even

    // 3. CRTC (80x25, 9x16 cells) — standard mode-3 timing.
    outb(VGA_CRTC, 0x00); outb(VGA_CRTC + 1, 0x5F); // h total
    outb(VGA_CRTC, 0x01); outb(VGA_CRTC + 1, 0x4F); // h displayed
    outb(VGA_CRTC, 0x02); outb(VGA_CRTC + 1, 0x50); // h blank start
    outb(VGA_CRTC, 0x03); outb(VGA_CRTC + 1, 0x82); // h blank end
    outb(VGA_CRTC, 0x04); outb(VGA_CRTC + 1, 0x55); // h sync start
    outb(VGA_CRTC, 0x05); outb(VGA_CRTC + 1, 0x81); // h sync end
    outb(VGA_CRTC, 0x06); outb(VGA_CRTC + 1, 0xBF); // v total
    outb(VGA_CRTC, 0x07); outb(VGA_CRTC + 1, 0x1F); // overflow
    outb(VGA_CRTC, 0x08); outb(VGA_CRTC + 1, 0x00); // preset row scan
    outb(VGA_CRTC, 0x09); outb(VGA_CRTC + 1, 0x4F); // max scan line
    outb(VGA_CRTC, 0x0A); outb(VGA_CRTC + 1, 0x0C); // cursor start
    outb(VGA_CRTC, 0x0B); outb(VGA_CRTC + 1, 0x0E); // cursor end
    outb(VGA_CRTC, 0x0C); outb(VGA_CRTC + 1, 0x00); // start addr high
    outb(VGA_CRTC, 0x0D); outb(VGA_CRTC + 1, 0x00); // start addr low
    outb(VGA_CRTC, 0x0E); outb(VGA_CRTC + 1, 0x00); // cursor loc high
    outb(VGA_CRTC, 0x0F); outb(VGA_CRTC + 1, 0x00); // cursor loc low
    outb(VGA_CRTC, 0x10); outb(VGA_CRTC + 1, 0x9C); // v sync start
    outb(VGA_CRTC, 0x11); outb(VGA_CRTC + 1, 0x8E); // v sync end
    outb(VGA_CRTC, 0x12); outb(VGA_CRTC + 1, 0x8F); // v displayed
    outb(VGA_CRTC, 0x13); outb(VGA_CRTC + 1, 0x28); // offset (line length)
    outb(VGA_CRTC, 0x14); outb(VGA_CRTC + 1, 0x40); // underline
    outb(VGA_CRTC, 0x15); outb(VGA_CRTC + 1, 0x96); // v blank start
    outb(VGA_CRTC, 0x16); outb(VGA_CRTC + 1, 0xB9); // v blank end
    outb(VGA_CRTC, 0x17); outb(VGA_CRTC + 1, 0xA3); // mode control

    // 4. Clear the text buffer to blank black cells (attribute 0x0F on black).
    volatile unsigned short* text = (volatile unsigned short*)0xB8000;
    for (int i = 0; i < 80 * 25; ++i)
    {
        text[i] = 0x0F20; // space with attribute 0x0F
    }
}
