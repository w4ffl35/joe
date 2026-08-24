// SPDX-License-Identifier: GPL-3.0
//
// fb.c — linear framebuffer renderer using the REAL framebuffer address from
// the multiboot2 info structure (passed by GRUB in ebx, saved by boot.S).
//
// This is the reliable display path for VirtualBox: GRUB's gfxterm sets a
// VBE linear framebuffer mode, and the multiboot2 "framebuffer info" tag
// carries the actual framebuffer address/pitch/size. We parse that tag and
// draw the message as 32bpp pixels — no reliance on a guessed constant base
// and no VGA text-plane (which VirtualBox renders unreliably after handoff).
//
// The QEMU -kernel (PVH) path has no multiboot info, so fb_ready() returns 0
// there and the kernel falls back to the VGA text buffer.
//
// Exposed to Curlee via extern fn (symbols match 1:1):
//   long long fb_ready(void);                    // 1 if a usable framebuffer
//   void fb_draw_char(long long ch, long long x, long long y, long long scale);
//     // draws one 5x7 glyph (scaled) at pixel (x,y) in orange.

static unsigned int fb_addr = 0;
static unsigned int fb_pitch = 0;
static unsigned int fb_width = 0;
static unsigned int fb_height = 0;

long long fb_ready(void)
{
    return (fb_addr != 0) ? 1 : 0;
}

// Phase-1 limitation: the multiboot2 framebuffer info (the real address GRUB
// set) is passed to a 64-bit ELF entry in registers GRUB does not set for
// long-mode entries (ebx is only guaranteed for 32-bit multiboot entries),
// and Curlee's codegen cannot read it. Until that gap is closed, fb_init()
// is a no-op: fb_ready() returns 0 and the kernel falls back to the VGA text
// buffer (renders correctly under QEMU). The glyph renderer below is kept so
// a future boot stub that exposes the framebuffer address can enable it.
void fb_init(void)
{
    fb_addr = 0;
}

// --- 5x7 glyphs (bit 6 = leftmost pixel of the row) -----------------------
static unsigned char glyph_row(char c, int row)
{
    switch (c)
    {
    case 'H': {
        static const unsigned char g[7] = {0x44, 0x44, 0x44, 0x7C, 0x44, 0x44, 0x44};
        return (row < 7) ? g[row] : 0;
    }
    case 'e': {
        static const unsigned char g[7] = {0x38, 0x44, 0x7C, 0x40, 0x38, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'l': {
        static const unsigned char g[7] = {0x20, 0x20, 0x20, 0x20, 0x38, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'o': {
        static const unsigned char g[7] = {0x38, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'W': {
        static const unsigned char g[7] = {0x44, 0x44, 0x44, 0x54, 0x6C, 0x44, 0x44};
        return (row < 7) ? g[row] : 0;
    }
    case 'r': {
        static const unsigned char g[7] = {0x18, 0x20, 0x20, 0x20, 0x70, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'd': {
        static const unsigned char g[7] = {0x08, 0x08, 0x38, 0x48, 0x48, 0x38, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'f': {
        static const unsigned char g[7] = {0x30, 0x48, 0x70, 0x40, 0x40, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'm': {
        static const unsigned char g[7] = {0x54, 0x54, 0x54, 0x54, 0x6C, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'C': {
        static const unsigned char g[7] = {0x38, 0x44, 0x40, 0x40, 0x38, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case 'u': {
        static const unsigned char g[7] = {0x28, 0x48, 0x48, 0x48, 0x38, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case '!': {
        static const unsigned char g[7] = {0x20, 0x20, 0x20, 0x00, 0x20, 0x00, 0x00};
        return (row < 7) ? g[row] : 0;
    }
    case ' ': {
        return 0;
    }
    default:
        return 0;
    }
}

void fb_draw_char(long long ch, long long x, long long y, long long scale)
{
    if (!fb_addr)
    {
        return;
    }
    if (scale < 1)
    {
        scale = 1;
    }
    for (int row = 0; row < 7; ++row)
    {
        unsigned char bits = glyph_row((char)ch, row);
        for (int col = 0; col < 5; ++col)
        {
            int on = (bits >> (4 - col)) & 1;
            if (!on)
            {
                continue;
            }
            // Draw a scale x scale block of orange pixels.
            for (int dy = 0; dy < scale; ++dy)
            {
                for (int dx = 0; dx < scale; ++dx)
                {
                    unsigned int py = (unsigned int)(y + row * scale + dy);
                    unsigned int px = (unsigned int)(x + col * scale + dx);
                    if (py >= fb_height || px >= fb_width)
                    {
                        continue;
                    }
                    volatile unsigned int* p =
                        (volatile unsigned int*)(fb_addr + py * fb_pitch + px * 4);
                    *p = 0x00FF8800; // orange
                }
            }
        }
    }
}
