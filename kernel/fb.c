// SPDX-License-Identifier: GPL-3.0
//
// fb.c — RAW BUILD-GEOMETRY SHIM for the Curlee framebuffer blitter
// (gh issue #13). The blitter-loop + event-loop logic, the tool-call queue,
// AND the small mutable blitter state all moved to genuine Curlee
// (kernel/fb.curlee): the tool ring (tool_kinds/tool_args/tool_head/
// tool_count), the loop counters (loop_frame/loop_drained), the ring
// bookkeeping (ring_active/ring_slot) and the draw-target indirection
// (fb_draw_target/fb_target_stride) are now Curlee `static` module state
// (the toolchain gained static + [T; N] arrays). The runtime-address
// physical-memory moves are now Curlee COMPILER BUILTINS (phys_write_u32 /
// phys_read_u8/u32 — inline volatile stores/loads in the freestanding
// codegen), so the C definitions of phys_write_u32 / fb_mem_read_u32 were
// deleted.
//
// What remains here, and ONLY why (per docs/c-boundary-policy.md §1 — "No
// logic in C — only I/O touches and raw memory moves"):
//   - fb_addr/fb_pitch/fb_width/fb_height: C-visible .data globals filled by
//     kernel/mb2_state.c and kernel/vbe_state.c (the multiboot2-tag / VBE-
//     probe state shims); the Curlee blitter reads them through the 4
//     getters below.
//   - The two LARGE static buffers — the 128x128 asset region and the 2-slot
//     640x480 frame ring — compiled OUT on the PVH build (JOE_PVH_BOOT):
//     QEMU's `-kernel` PVH loader refuses ELFs whose LOAD segment (file +
//     BSS) exceeds a hard budget (verified empirically in
//     docs/phase2c-report.md §4.3: even a 64 KB static buffer makes the BIOS
//     hang with no serial; the PVH kernel's BSS+stack is ~45 KB today).
//     Curlee has no conditional compilation and BOTH builds compile the SAME
//     merged kernel.c (scripts/build-kernel.sh), so a full-size Curlee
//     static array would land in the PVH kernel's BSS unconditionally. The
//     C `#ifndef JOE_PVH_BOOT` is the ONLY way to size them per build. This
//     is the sole reason these two buffers stay in C; every other byte of
//     the former shim moved to Curlee.
//   - The compile-time build geometry (the ONLY #ifdefs left in this file):
//     fb_pvh_build() (1 on the PVH build, 0 on the GRUB build) and the two
//     base-address getters (fb_asset_region_base_get /
//     fb_frame_ring_slot_base, 0 on the PVH 1-byte stubs) that let the
//     Curlee layer address the C-owned arrays as runtime addresses.
//
// The weak `mb2_info_addr` default lives in kernel/mb2_state.c (its genuine
// owner — the module whose mb2_info_addr_get reads it).

#include <stdint.h>

// Framebuffer state. Zero until the multiboot2 framebuffer tag (GRUB path)
// or the Bochs VBE probe (PVH path) is parsed.
unsigned int fb_addr = 0;
unsigned int fb_pitch = 0;
unsigned int fb_width = 0;
unsigned int fb_height = 0;

// ---------------------------------------------------------------------------
// Phase 2c: static asset region + frame ring (no malloc anywhere)
// ---------------------------------------------------------------------------
// PVH size constraint (critical): QEMU's `-kernel` PVH loader refuses ELFs
// whose LOAD segment (file + BSS) exceeds a hard budget (a large BSS makes
// the BIOS hang with no serial). The PVH path has NO framebuffer (VGA text +
// serial fallback) so it never uses the ring/region; the GRUB path HAS the
// linear framebuffer and is where the ring actually flips. So the buffers
// are compiled OUT on the PVH build (JOE_PVH_BOOT, kernel.elf/kernel-smoke
// .elf) and IN at full size on the GRUB build (no macro). The Curlee layer
// addresses them via fb_asset_region_base_get / fb_frame_ring_slot_base
// (0 on the PVH stubs, so the blitter gates never activate there).
#ifndef JOE_PVH_BOOT
// 128x128 32bpp RAW pixel store (fb_asset_region_base exposes it).
#define ASSET_REGION_W 128
#define ASSET_REGION_H 128
static unsigned int asset_region[ASSET_REGION_W * ASSET_REGION_H];

// Double-buffer ring of full-frame 32bpp surfaces, each up to 640x480 — the
// exact size the multiboot2 framebuffer request tag (boot.S) asks GRUB for,
// so the ring activates on the GRUB path and the Curlee fb_present()
// performs a REAL flip. GRUB loads this 32-bit ELF with no PVH size limit.
#define FRAME_RING_SLOTS     2
#define FRAME_RING_MAX_W     640
#define FRAME_RING_MAX_H     480
#define FRAME_RING_SLOT_BYTES (FRAME_RING_MAX_W * FRAME_RING_MAX_H * 4)
static unsigned int frame_ring[FRAME_RING_SLOTS][FRAME_RING_MAX_W * FRAME_RING_MAX_H];
#else
// PVH path: 1-byte stubs so the PVH LOAD budget is untouched. The accessor
// externs still exist and return 0 (the Curlee gates see a 0-sized region
// and never activate).
#define ASSET_REGION_W 0
#define ASSET_REGION_H 0
#define FRAME_RING_SLOTS     0
#define FRAME_RING_MAX_W     0
#define FRAME_RING_MAX_H     0
#define FRAME_RING_SLOT_BYTES 0
static unsigned int asset_region[1];
static unsigned int frame_ring[1][1];
#endif

// ---------------------------------------------------------------------------
// Compile-time build geometry (the ONLY #ifdefs left): the Curlee layer reads
// fb_pvh_build() to return 0 vs. the pure region geometry per build
// (fb_asset_region_w/h in kernel/fb.curlee), and the two base getters for
// the C-owned buffers' runtime addresses (0 on the PVH stub).
// ---------------------------------------------------------------------------

long long fb_pvh_build(void)
{
#ifdef JOE_PVH_BOOT
    return 1;
#else
    return 0;
#endif
}

long long fb_asset_region_base_get(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (long long)(unsigned long)asset_region;
#endif
}

// Base address of frame-ring slot `slot` (0 on the PVH stub, or for an
// out-of-range slot). The Curlee fb_ring_activate / fb_ring_advance re-point
// the draw target at this address (bounds-checked as defense-in-depth).
long long fb_frame_ring_slot_base(long long slot)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (slot >= 0 && slot < FRAME_RING_SLOTS)
    {
        return (long long)(unsigned long)frame_ring[(unsigned int)slot];
    }
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Framebuffer state getters (the Curlee blitter reads the globals through
// these — mb2_state.c / vbe_state.c fill them).
// ---------------------------------------------------------------------------

long long fb_addr_get(void) { return (long long)fb_addr; }
long long fb_pitch_get(void) { return (long long)fb_pitch; }
long long fb_width_get(void) { return (long long)fb_width; }
long long fb_height_get(void) { return (long long)fb_height; }
