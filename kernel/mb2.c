// SPDX-License-Identifier: GPL-3.0
//
// mb2.c — multiboot2 info structure parser (framebuffer tag).
//
// Phase 2e: GRUB hands the kernel a multiboot2 info structure whose pointer is
// passed in %ebx on entry. kernel/boot.S saves that pointer into the .data
// global `mb2_info_addr` (before zeroing .bss), and this module walks the info
// structure to find the framebuffer tag (type 8), filling the framebuffer
// globals that kernel/fb.c's blitter uses.
//
// Freestanding: no libc — only volatile reads from physical memory (the kernel
// runs identity-mapped, so the info structure and framebuffer addresses are
// directly writable, exactly like the VGA text buffer at 0xB8000).
//
// Multiboot2 info layout (spec):
//   offset 0: u32 total_size        (whole structure, 8-byte aligned tags)
//   offset 4: u32 reserved          (must be 0)
//   offset 8: tags...               each: u32 type, u32 size, payload
// Framebuffer tag (type 8) payload (packed):
//   u64 framebuffer_addr
//   u32 framebuffer_pitch
//   u32 framebuffer_width
//   u32 framebuffer_height
//   u8  bpp
//   u8  type (0=indexed, 1=RGB, 2=EGA text)
//   ... reserved ...
//
// Only 32bpp linear framebuffers are supported (the blitter writes U32s).

// Set by kernel/boot.S on entry (GRUB multiboot2 info pointer; 0 on the
// QEMU PVH path which has no multiboot info).
extern unsigned long long mb2_info_addr;

// Framebuffer globals owned by kernel/fb.c (non-static so this module can
// fill them).
extern unsigned int fb_addr;
extern unsigned int fb_pitch;
extern unsigned int fb_width;
extern unsigned int fb_height;

// Multiboot2 framebuffer tag (type 8), packed per the spec (GRUB multiboot2
// 3.6.7). The common tag header is u16 type + u16 flags + u32 size; the
// framebuffer payload after it is:
//   u64 framebuffer_addr, u32 framebuffer_pitch, u32 framebuffer_width,
//   u32 framebuffer_height, u8 bpp, u8 type, u8 reserved (pads to 32 bytes).
struct mb2_fb_tag
{
    unsigned int type;             /* 8 (u16 type | u16 flags<<16) */
    unsigned int size;             /* tag size (0x20 = 32) */
    unsigned long long fb_addr;    /* physical framebuffer address (u64) */
    unsigned int fb_pitch;         /* bytes per scanline (u32) */
    unsigned int fb_width;         /* pixels (u32) */
    unsigned int fb_height;        /* scanlines (u32) */
    unsigned char bpp;             /* bits per pixel (u8) */
    unsigned char fb_type;         /* 1 = RGB (u8) */
    unsigned char reserved;        /* u8 pad to 32 bytes */
} __attribute__((packed));

// Parse the multiboot2 info structure at mb2_info_addr and fill the
// framebuffer globals. Returns 1 if a 32bpp framebuffer tag was found and the
// globals were set; 0 otherwise (no info, or no usable framebuffer tag).
int mb2_parse(void)
{
    // The boot info pointer is TRUSTED: it comes from the 32-bit multiboot2
    // entry's %ebx (spec-guaranteed), stored by boot.S before curlee_main.
    // The guard is minimal: non-zero and within the 32-bit address space
    // (GRUB places the structure right after the kernel image, e.g. 0x109A78
    // for a kernel loaded at 0x100000 — NOT below 1 MiB). The total_size
    // check below bounds every walk, so a bad pointer can never run away.
    if (mb2_info_addr == 0 || mb2_info_addr >= 0x100000000ULL)
    {
        return 0;
    }
    const volatile unsigned char* base =
        (const volatile unsigned char*)(unsigned long)mb2_info_addr;
    const unsigned int total = *(const volatile unsigned int*)(base + 0);
    if (total < 16 || total > 0x10000)
    {
        return 0;
    }
    const unsigned long long end = mb2_info_addr + total;
    unsigned long long p = mb2_info_addr + 8; /* skip total_size + reserved */

    while (p + 8 <= end)
    {
        const volatile unsigned char* tag =
            (const volatile unsigned char*)(unsigned long)p;
        const unsigned int type = *(const volatile unsigned int*)(tag + 0);
        const unsigned int size = *(const volatile unsigned int*)(tag + 4);
        if (size == 0)
        {
            break; /* malformed guard */
        }
        if (type == 8) /* framebuffer */
        {
            const struct mb2_fb_tag* t =
                (const struct mb2_fb_tag*)(unsigned long)p;
            // Only refine the framebuffer state when the tag is genuinely
            // usable: 32bpp AND a non-zero framebuffer address. The pointer
            // comes from the trusted 32-bit entry, but keep the gate anyway
            // (defense-in-depth against a bogus tag from a corrupt info).
            if (t->bpp == 32 && t->fb_addr != 0)
            {
                fb_addr = (unsigned int)t->fb_addr;
                fb_pitch = (unsigned int)t->fb_pitch;
                fb_width = (unsigned int)t->fb_width;
                fb_height = (unsigned int)t->fb_height;
                return 1;
            }
            return 0; /* framebuffer tag present but not usable */
        }
        /* Tags are 8-byte aligned. */
        p += (unsigned long long)((size + 7) & ~7U);
    }
    return 0;
}
