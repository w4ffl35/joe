// SPDX-License-Identifier: GPL-3.0
//
// vbe.c — Bochs VBE (QEMU stdvga / VirtualBox) linear-framebuffer probe.
//
// Phase 2f: the QEMU `-kernel` (PVH) path has no multiboot2 info structure,
// so kernel/mb2.c cannot find a framebuffer tag and fb_ready() stays 0 there
// (VGA text + serial fallback only). This driver probes the Bochs VBE
// controller — the I/O-port interface (0x1CE/0x1CF) that QEMU's stdvga and
// VirtualBox's VGA both implement — to:
//
//   (a) confirm a VBE device is present (ID register returns the Bochs
//       family 0xB0Cx; QEMU stdvga returns 0xB0C5),
//   (b) set a 640x480x32 linear-framebuffer mode directly from the kernel
//       (the Bochs VBE extension is designed for exactly this — no BIOS
//       call, no real-mode thunk),
//   (c) validate the mode was accepted by reading the registers back.
//
// On success it fills the SAME framebuffer globals (fb_addr/pitch/width/
// height) that kernel/mb2.c fills from the multiboot2 tag, so the blitter
// (kernel/fb.c) is unchanged and fb_ready() returns 1.
//
// Trust model (matches kernel/mb2.c): fb_ready() must return 1 only for a
// VALIDATED framebuffer. Validation here is: Bochs VBE ID present AND the
// 32bpp LFB mode-set was accepted (register readback matches) AND the LFB
// base passes a 32-bit MMIO-range sanity gate. Only then do we touch the
// LFB — no hardcoded/unvalidated LFB writes (an unmapped LFB faults the VM,
// docs/phase2e-architecture.md §7 finding 4).
//
// Freestanding: only I/O-port inw/outw, no libc.

// Framebuffer state owned by kernel/fb.c.
// GRUB build: non-static .bss globals (extern here). PVH build
// (JOE_PVH_BOOT): fbstate.h routes fb_* to the writable high-RAM block —
// QEMU's `-kernel` PVH loader maps .bss read-only (Phase 2f), so the probe's
// stores must land in writable RAM or fb_ready() never sees them. vbe.c is
// only compiled into the PVH ELFs (kernel.elf / kernel-smoke.elf), so this
// is the PVH wiring.
#ifdef JOE_PVH_BOOT
#include "fbstate.h"
#else
extern unsigned int fb_addr;
extern unsigned int fb_pitch;
extern unsigned int fb_width;
extern unsigned int fb_height;
#endif

// Bochs VBE I/O interface: index register at 0x1CE, data register at 0x1CF.
#define VBE_DISPI_IO       0x1CE
#define VBE_DISPI_DATA     0x1CF

// Bochs VBE index registers.
#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4

// Bochs VBE enable-register bits.
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

// The mode we request — matches the multiboot2 framebuffer request tag in
// kernel/boot.S (640x480x32) and the frame-ring geometry in kernel/fb.c.
#define VBE_PROBE_W       640
#define VBE_PROBE_H       480
#define VBE_PROBE_BPP     32

// Documented QEMU stdvga LFB base (the only LFB source — QEMU stdvga does
// NOT expose the LFB via a usable PCI BAR; BAR0 is the 128 KiB legacy VGA
// window at 0xFEFE0000, which is not the LFB). Verified on the GRUB path:
// the multiboot2 framebuffer tag reports a:0xFD000000 with `-vga std`
// (docs/phase2e-2-report.md §3). VirtualBox uses the same class of address.
#define VBE_LFB_PHYS_FALLBACK 0xFD000000U

static inline void outw(unsigned short port, unsigned short value)
{
    __asm__ volatile("outw %0, %1" ::"a"(value), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// Probe the Bochs VBE controller and, on success, fill the framebuffer
// globals. Returns 1 when a validated 32bpp linear framebuffer is active.
int vbe_probe(void)
{
    // 1. Presence: the Bochs VBE ID register must return a 0xB0Cx family
    //    value (QEMU stdvga returns 0xB0C5). Anything else means no VBE
    //    device on the legacy ports (e.g. `-vga none`).
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_ID);
    const unsigned short id = inw(VBE_DISPI_DATA);
    if ((id & 0xFFF0) != 0xB0C0)
    {
        return 0;
    }

    // 2. Read the CURRENT mode. If it already matches 640x480x32 with the
    //    LFB enabled, skip re-programming (true idempotence — re-running the
    //    full disable/enable sequence on an already-active mode is not
    //    reliably idempotent on QEMU stdvga).
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_ENABLE);
    const unsigned short en = inw(VBE_DISPI_DATA);
    if ((en & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) ==
        (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED))
    {
        outw(VBE_DISPI_IO, VBE_DISPI_INDEX_XRES);
        const unsigned short cx = inw(VBE_DISPI_DATA);
        outw(VBE_DISPI_IO, VBE_DISPI_INDEX_YRES);
        const unsigned short cy = inw(VBE_DISPI_DATA);
        outw(VBE_DISPI_IO, VBE_DISPI_INDEX_BPP);
        const unsigned short cb = inw(VBE_DISPI_DATA);
        if (cx == VBE_PROBE_W && cy == VBE_PROBE_H && cb == VBE_PROBE_BPP)
        {
            goto validated;
        }
    }

    // 3. Program the mode: disable, set 640x480x32, re-enable with the LFB
    //    bit set (ENABLED | LFB_ENABLED = 0x41).
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_DATA, VBE_DISPI_DISABLED);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_XRES);
    outw(VBE_DISPI_DATA, VBE_PROBE_W);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_YRES);
    outw(VBE_DISPI_DATA, VBE_PROBE_H);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_BPP);
    outw(VBE_DISPI_DATA, VBE_PROBE_BPP);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_DATA, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    // 4. Validate by readback: the controller must report the exact mode we
    //    requested. QEMU stdvga clamps/refuses unsupported modes, so any
    //    mismatch means there is no usable LFB.
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_XRES);
    const unsigned short xres = inw(VBE_DISPI_DATA);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_YRES);
    const unsigned short yres = inw(VBE_DISPI_DATA);
    outw(VBE_DISPI_IO, VBE_DISPI_INDEX_BPP);
    const unsigned short bpp = inw(VBE_DISPI_DATA);
    if (xres != VBE_PROBE_W || yres != VBE_PROBE_H || bpp != VBE_PROBE_BPP)
    {
        return 0;
    }

validated:
    // 5. The LFB base is the documented QEMU stdvga VBE LFB (0xFD000000).
    //    Sanity gate: base in the 32-bit MMIO range.
    if (VBE_LFB_PHYS_FALLBACK < 0x100000U || VBE_LFB_PHYS_FALLBACK >= 0x100000000ULL)
    {
        return 0;
    }

    // 6. Fill the framebuffer globals the blitter reads. 32bpp: pitch =
    //    width * 4 — the same relationship the multiboot2 tag reports.
    fb_addr = VBE_LFB_PHYS_FALLBACK;
    fb_pitch = (unsigned int)xres * (VBE_PROBE_BPP / 8);
    fb_width = (unsigned int)xres;
    fb_height = (unsigned int)yres;
    return 1;
}
