// SPDX-License-Identifier: GPL-3.0
//
// fb.c — RAW STATE + MEMORY-MOVE SHIM for the Curlee framebuffer blitter
// (gh issue #13). The blitter-loop + event-loop logic migrated to
// kernel/fb.curlee (fb_clear/fb_pixel/fb_fill_rect/fb_line/fb_blit_asset/
// fb_present, the tool-call queue, the loop control). What remains here is
// ONLY state and raw memory moves Curlee cannot own (no module-level
// globals, no arrays — the vbe_state.c / mb2_state.c / net_stack.c
// precedent), per docs/c-boundary-policy.md §1 ("No logic in C — only I/O
// touches and raw memory moves"):
//   - ALL mutable state: fb_addr/pitch/width/height (NON-static so
//     kernel/mb2_state.c and kernel/vbe_state.c fill them from the trusted
//     framebuffer tag / VBE probe), the static asset region + frame ring
//     (JOE_PVH_BOOT conditional), ring_active/ring_slot, the draw-target
//     indirection (fb_draw_target/fb_target_stride), and the tool-call ring
//     (tool_queue + head/count + loop counters). Curlee reads/writes it all
//     through the thin extern window kernel/fb.curlee declares.
//   - The raw runtime-address memory moves: phys_write_u32 (the write
//     counterpart of mb2_state.c's phys_read_u8/u32 — the curlee #279
//     family, extended with writes; the curlee runtime does not carry the
//     symbol) and fb_mem_read_u32 (bit-preserving volatile load — the draw
//     target / asset sources are runtime addresses, which a
//     compile-time-literal Phys<T> cannot address).
//   - The compile-time build geometry: fb_pvh_build() (1 on the PVH build,
//     0 on the GRUB build — the JOE_PVH_BOOT discrimination) and
//     fb_asset_region_base_get(). The only #ifdef left in this file.
//
// The weak `mb2_info_addr` default moved to kernel/mb2_state.c (its genuine
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
// .elf) and IN at full size on the GRUB build (no macro).
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

// Ring bookkeeping + target indirection: the blitter writes to fb_draw_target
// (an abstract 32bpp surface with its own stride) instead of the visible
// framebuffer when the ring is active. Before the first present the target IS
// the visible framebuffer (fb_addr) — 2a/2e single-buffer behavior exactly.
static unsigned int ring_active = 0;
static unsigned int ring_slot = 0;
static volatile unsigned int* fb_draw_target = 0;
static unsigned int fb_target_stride = 0;

// Phase 2b: tool-call ring + loop state (raw ring/state owner; the enqueue /
// drain LOGIC is Curlee's fb_tool_enqueue / fb_run_loop). Geometry MUST stay
// in sync with assets.curlee: tool_queue_slots()==8, tool_slot_bytes()==16 —
// canvas_test.curlee §12 asserts the exact values.
#define TOOL_QUEUE_SLOTS 8
#define TOOL_SLOT_BYTES  16
struct tool_slot
{
    long long kind;   // kind 0 = null/empty
    long long arg;
};
static struct tool_slot tool_queue[TOOL_QUEUE_SLOTS];
static unsigned int tool_head = 0;    // next slot to write (producer cursor)
static unsigned int tool_count = 0;   // live (not-yet-drained) intents
static unsigned long long loop_frame = 0;  // the 60 FPS loop fuel
static unsigned int loop_drained = 0;      // intents drained by the last tick

// ---------------------------------------------------------------------------
// Compile-time build geometry (the ONLY #ifdef left): the Curlee layer reads
// fb_pvh_build() to return 0 vs. the pure region geometry per build
// (fb_asset_region_w/h in kernel/fb.curlee), and fb_asset_region_base_get()
// for the region's address (0 on the PVH stub).
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

// ---------------------------------------------------------------------------
// Framebuffer state getters
// ---------------------------------------------------------------------------

long long fb_addr_get(void) { return (long long)fb_addr; }
long long fb_pitch_get(void) { return (long long)fb_pitch; }
long long fb_width_get(void) { return (long long)fb_width; }
long long fb_height_get(void) { return (long long)fb_height; }

// ---------------------------------------------------------------------------
// Draw-target indirection (state only; the wiring LOGIC is Curlee's
// fb_wire_draw_target / fb_present)
// ---------------------------------------------------------------------------

long long fb_draw_target_addr(void)
{
    return (long long)(unsigned long)fb_draw_target;
}

long long fb_target_stride_get(void)
{
    return (long long)fb_target_stride;
}

void fb_target_set(long long addr, long long stride)
{
    fb_draw_target = (volatile unsigned int*)(unsigned long)addr;
    fb_target_stride = (unsigned int)stride;
}

// ---------------------------------------------------------------------------
// Frame-ring transitions (the flip LOGIC — fit gate, copy, advance decision
// — is Curlee's fb_present; these are the raw state changes)
// ---------------------------------------------------------------------------

long long fb_ring_active_get(void) { return ring_active ? 1 : 0; }
long long fb_ring_slot_get(void) { return (long long)ring_slot; }

// Activate on the first present: ring_active=1, slot=0, draw target re-points
// at frame_ring[0] with stride 640*4. Returns 1 on a real activation; on the
// PVH build (ring compiled out) this is a no-op returning 0, so the Curlee
// fb_present stays single-buffered — exactly the old #ifdef JOE_PVH_BOOT
// present's early return.
long long fb_ring_activate(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    ring_active = 1;
    ring_slot = 0;
    fb_draw_target = frame_ring[ring_slot];
    fb_target_stride = FRAME_RING_MAX_W * 4;
    return 1;
#endif
}

// Advance after a flip: 0 -> 1 -> 0, matching assets.curlee's frame_ring_next
// for FRAME_RING_SLOTS == 2 (C is free to use %; the division-free rule
// applies to the Curlee verifier fragment only). No-op on the PVH build.
void fb_ring_advance(void)
{
#ifdef JOE_PVH_BOOT
    return;
#else
    ring_slot = (ring_slot + 1) % FRAME_RING_SLOTS;
    fb_draw_target = frame_ring[ring_slot];
    fb_target_stride = FRAME_RING_MAX_W * 4;
#endif
}

// ---------------------------------------------------------------------------
// Tool-call ring: raw slot store + counter getters/setters (the enqueue /
// drain LOGIC is Curlee's fb_tool_enqueue / fb_run_loop)
// ---------------------------------------------------------------------------

// Raw write into one ring slot (the Curlee layer computes the slot index
// 0..7 from the verified geometry). Bounds-checked as defense-in-depth.
void fb_tool_slot_set(long long slot, long long kind, long long arg)
{
    if (slot >= 0 && slot < TOOL_QUEUE_SLOTS)
    {
        tool_queue[(unsigned int)slot].kind = kind;
        tool_queue[(unsigned int)slot].arg = arg;
    }
}

long long fb_tool_head(void) { return (long long)tool_head; }
void fb_tool_head_set(long long h) { tool_head = (unsigned int)h; }
long long fb_tool_count(void) { return (long long)tool_count; }
void fb_tool_count_set(long long c) { tool_count = (unsigned int)c; }
long long fb_tool_drained_get(void) { return (long long)loop_drained; }
void fb_tool_drained_set(long long d) { loop_drained = (unsigned int)d; }

// ---------------------------------------------------------------------------
// Loop control: frame-counter getters/setters + the full state reset (the
// loop POLICY is Curlee's main + fb_run_loop)
// ---------------------------------------------------------------------------

long long fb_loop_frame_get(void) { return (long long)loop_frame; }
void fb_loop_frame_set(long long f) { loop_frame = (unsigned long long)f; }

// Reset the frame counter, the tool ring, and (on the GRUB path) the frame
// ring back to single-buffer mode. Called once by Curlee main (fb_loop_init).
void fb_loop_state_reset(void)
{
    loop_frame = 0;
    tool_head = 0;
    tool_count = 0;
    loop_drained = 0;
    for (unsigned int i = 0; i < TOOL_QUEUE_SLOTS; ++i)
    {
        tool_queue[i].kind = 0;
        tool_queue[i].arg = 0;
    }
#ifndef JOE_PVH_BOOT
    ring_active = 0;
    ring_slot = 0;
    fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
    fb_target_stride = fb_pitch;
#endif
}

// ---------------------------------------------------------------------------
// Raw runtime-address memory moves: phys_write_u32 is the write counterpart
// of mb2_state.c's phys_read_u8/u32 (curlee #279's family, extended with
// writes; the curlee runtime rt.c does not carry the symbol). fb_mem_read_u32
// is the bit-preserving read side (named distinctly: mb2.curlee already
// declares phys_read_u32 returning U32, and the merged TU forbids duplicate
// externs). The draw target / asset sources are runtime addresses — never
// compile-time literals, so Phys<T> cannot address them.
// ---------------------------------------------------------------------------

void phys_write_u32(long long addr, long long value)
{
    *(volatile uint32_t*)(uintptr_t)addr = (uint32_t)value;
}

long long fb_mem_read_u32(long long addr)
{
    return (long long)*(volatile uint32_t*)(uintptr_t)addr;
}
