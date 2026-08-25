// SPDX-License-Identifier: GPL-3.0
//
// fb.c — linear framebuffer software blitter for the Phase 2 renderer.
//
// This is the IMPERATIVE layer of the renderer: it owns the mutable
// framebuffer state (address, pitch, dimensions) and executes the pixel
// loops. Curlee (canvas.curlee / glyphs.curlee / assets.curlee) computes the
// geometry and intent; this file executes the pixels. The Curlee layer is
// pure and verified; this layer is the only place (volatile) framebuffer
// writes happen.
//
// Exposed to Curlee via extern fn (symbols match 1:1):
//   long long fb_ready(void);
//   void fb_clear(long long color);
//   void fb_pixel(long long x, long long y, long long color);
//   void fb_fill_rect(long long x, long long y, long long w, long long h,
//                     long long color);
//   void fb_line(long long x0, long long y0, long long x1, long long y1,
//                long long color);
//   void fb_draw_char(long long ch, long long x, long long y, long long scale);
//     // Phase-1 compat: draws in orange (fixed color).
//   void fb_draw_char_color(long long ch, long long x, long long y,
//                           long long scale, long long color);
//   void fb_blit_asset(long long src, long long src_w, long long src_h,
//                      long long dst_x, long long dst_y);
//     // Blits a 32bpp RAW pixel buffer (src points at a uint32_t array of
//     // src_w * src_h pixels) to the framebuffer at (dst_x, dst_y).
//   void fb_present(void);   // flip: back-buffer ring -> visible FB
//
// Phase 2c — memory & asset management contracts. Accessor/geometry externs
// the Curlee layer reads to enforce the verified gates at run time, plus the
// ring/region state they describe:
//   long long fb_get_width(void);        // current FB width  (pixels)
//   long long fb_get_height(void);       // current FB height (pixels)
//   long long fb_asset_region_w(void);   // static asset region width
//   long long fb_asset_region_h(void);   // static asset region height
//   long long fb_asset_region_base(void);// base address of the asset region
//   long long fb_ring_active(void);      // 1 when the frame ring is active
//   long long fb_ring_slot(void);        // current back-buffer slot index
//
// Phase 2b 60 FPS event loop + kernel tool-call API (the loop is driven by
// Curlee's `main`; this file owns ALL the mutable loop state):
//   long long fb_loop_init(void);    // reset frame counter + tool ring
//   long long fb_loop_frame(void);   // current frame counter (loop fuel)
//   long long fb_tool_enqueue(long long kind, long long arg);
//     // Producer API: enqueue a (kind, arg) tool intent. 1 = ok, 0 = full.
//   long long fb_tool_drained(void); // intents consumed by the last tick
//   long long fb_tool_pending(void); // intents currently queued
//   void fb_run_loop(void);          // one tick: drain ring + advance frame
//
// Why Curlee drives the loop: the freestanding codegen emits every non-main
// function with `static` linkage, so a separate TU (fb.c) cannot call
// `curlee_render_frame`. Curlee's `main` is the one exported symbol, so it
// runs the deterministic while-loop and calls fb_run_loop() per frame.
//
// Tool-call ring geometry MUST stay in sync with assets.curlee
// (tool_queue_slots()==8, tool_slot_bytes()==16); canvas_test.curlee §12
// asserts both sides agree.
//
// Bounds discipline: every primitive bounds-checks against fb_width /
// fb_height (defense-in-depth over the Curlee geometry layer, which is
// provably in-bounds). Out-of-bounds writes are impossible by construction.

// Framebuffer state. NON-static so kernel/mb2.c (the multiboot2 parser) can
// fill them from the trusted framebuffer tag captured by the 32-bit boot stub.
// The blitter below reads them. Zero until fb_init() parses the multiboot2
// framebuffer tag.
unsigned int fb_addr = 0;
unsigned int fb_pitch = 0;
unsigned int fb_width = 0;
unsigned int fb_height = 0;

// ---------------------------------------------------------------------------
// Phase 2c: static asset region + frame ring (no malloc anywhere)
// ---------------------------------------------------------------------------
//
// The Curlee layer (assets.curlee) owns the pure geometry for these two
// static buffers; this file owns the actual storage. The values MUST stay in
// sync — canvas_test.curlee §14/§15 asserts the pure constants
// (asset_region_w/h, frame_ring_slots/max_w/max_h/slot_bytes) and a drift
// fails `make canvas-run` before any boot.
//
// PVH size constraint (critical): QEMU's `-kernel` PVH loader refuses ELFs
// whose LOAD segment (file + BSS) exceeds a hard budget — empirically the
// image must stay under ~0x8000-0x10000 bytes of memsz beyond the base.
// The pristine kernel is already near that limit, so ANY large static buffer
// (a 128x128 asset region = 64 KB, a 2x320x240 frame ring = 614 KB) blows
// the PVH budget and QEMU silently fails to enter the kernel (verified: the
// BIOS hangs executing zeros — no exception, no serial).
//
// The PVH path (`kernel.elf`, qemu -kernel via crt0.S) has NO framebuffer
// (fb_ready()==0 — it falls back to VGA text + serial), so it never uses the
// ring/asset region. The GRUB path (`kernel-grub.elf`, ISO via boot.S + the
// 32-bit linker-grub.ld) HAS the linear framebuffer and is where the ring
// actually flips. Therefore: the buffers are compiled OUT on the PVH build
// (JOE_PVH_BOOT defined by the Makefile for kernel.elf/kernel-smoke.elf) and
// compiled IN at full size on the GRUB build (no macro) — no malloc anywhere,
// and the PVH gate stays green.
#ifndef JOE_PVH_BOOT
// ASSET REGION: a fixed 128x128 32bpp RAW pixel store the kernel can stage
// assets into (fb_blit_asset stages; fb_asset_region_base exposes it).
#define ASSET_REGION_W 128
#define ASSET_REGION_H 128
static unsigned int asset_region[ASSET_REGION_W * ASSET_REGION_H];

// FRAME RING: a double-buffer ring of full-frame 32bpp surfaces, each up to
// 640x480 — the exact size the multiboot2 framebuffer request tag (boot.S)
// asks GRUB for, so the ring activates on the GRUB framebuffer path and
// fb_present() performs a REAL flip. The kernel renders into the CURRENT
// BACK BUFFER (fb_draw_target + fb_target_stride); fb_present() copies it
// onto the visible framebuffer, then advances the ring slot. GRUB loads this
// 32-bit ELF with no PVH size limit, so the 2.4 MB ring BSS is fine here.
#define FRAME_RING_SLOTS     2
#define FRAME_RING_MAX_W     640
#define FRAME_RING_MAX_H     480
#define FRAME_RING_SLOT_BYTES (FRAME_RING_MAX_W * FRAME_RING_MAX_H * 4)
static unsigned int frame_ring[FRAME_RING_SLOTS][FRAME_RING_MAX_W * FRAME_RING_MAX_H];
#else
// PVH path (qemu -kernel, crt0.S): NO framebuffer and QEMU's PVH loader
// refuses ELFs whose LOAD segment (file + BSS) exceeds a hard budget
// (verified empirically: a large BSS makes the BIOS hang with no serial).
// So the ring/asset region are compile-time empty here. The accessor externs
// still exist and return 0, keeping the Curlee extern surface identical on
// both paths (the Curlee gates see a 0-sized region and never activate).
#define ASSET_REGION_W 0
#define ASSET_REGION_H 0
#define FRAME_RING_SLOTS     0
#define FRAME_RING_MAX_W     0
#define FRAME_RING_MAX_H     0
#define FRAME_RING_SLOT_BYTES 0
static unsigned int asset_region[1];
static unsigned int frame_ring[1][1];
#endif

// Ring bookkeeping: the current back-buffer slot (the one render_frame draws
// into) and whether the ring is active (fb_ring_active). The ring activates
// on the first fb_present() when the framebuffer fits a slot (Curlee's
// frame_ring_fits gates the geometry; this flag gates the flip).
static unsigned int ring_active = 0;
static unsigned int ring_slot = 0;

// Target indirection: the blitter writes to fb_draw_target (an abstract
// 32bpp surface with its own stride) instead of the visible framebuffer when
// the ring is active. Before the first present the target IS the visible
// framebuffer (fb_addr), preserving 2a/2e single-buffer behavior exactly.
// fb_target_stride is the byte stride of the active target (fb_pitch for the
// visible FB, FRAME_RING_MAX_W * 4 for a ring slot).
static volatile unsigned int* fb_draw_target = 0;
static unsigned int fb_target_stride = 0;

// kernel/mb2.c — parse the multiboot2 info structure captured by boot.S into
// the framebuffer globals above. Returns 1 on a usable 32bpp framebuffer tag.
int mb2_parse(void);

// Weak defaults so the QEMU PVH path (which links crt0.S, not boot.S) still
// links; boot.S's strong .data definition overrides this when present.
// mb2.c reads it to find the multiboot2 framebuffer tag (best-effort).
unsigned long long mb2_info_addr __attribute__((weak)) = 0;

long long fb_ready(void)
{
    return (fb_addr != 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Phase 2c accessor/geometry externs (called by Curlee render_frame to
// enforce the verified gates with the RUNTIME sizes)
// ---------------------------------------------------------------------------

long long fb_get_width(void)
{
    return (long long)fb_width;
}

long long fb_get_height(void)
{
    return (long long)fb_height;
}

long long fb_asset_region_w(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return ASSET_REGION_W;
#endif
}

long long fb_asset_region_h(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return ASSET_REGION_H;
#endif
}

long long fb_asset_region_base(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (long long)(unsigned long)asset_region;
#endif
}

long long fb_ring_active(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return ring_active ? 1 : 0;
#endif
}

long long fb_ring_slot(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (long long)ring_slot;
#endif
}

// Phase 2e-2: fb_init() activates the linear framebuffer by parsing the
// TRUSTED multiboot2 info structure captured by the 32-bit boot stub.
//
// GRUB enters a 32-bit ELF (kernel-grub.elf, as --32 + ld -m elf_i386) in
// 32-bit protected mode with %ebx = the boot info pointer (spec-guaranteed),
// which boot.S stores into mb2_info_addr. mb2_parse() walks that structure,
// finds the framebuffer tag (type 8), and fills fb_addr/pitch/width/height.
//
// No hardcoded VBE address is used: writing an LFB constant when no gfxterm
// framebuffer is actually mapped faults the VM (docs/phase2e-architecture.md
// §7 finding 4). The multiboot2 tag is the single source of truth; when it is
// absent (text-mode ISO, PVH path) fb_ready() stays 0 and the kernel falls
// back to VGA text + serial (all gates green).
void fb_init(void)
{
    mb2_parse();
#ifndef JOE_PVH_BOOT
    // Target indirection: before the frame ring activates, the draw target
    // IS the visible framebuffer (single-buffer 2a/2e behavior exactly).
    // fb_present() re-points fb_draw_target at the ring back buffers.
    fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
    fb_target_stride = fb_pitch;
#endif
}

// ---------------------------------------------------------------------------
// Pixel primitives
// ---------------------------------------------------------------------------

void fb_clear(long long color)
{
    if (!fb_draw_target)
    {
        return;
    }
    const unsigned int c = (unsigned int)color;
    volatile unsigned int* p = fb_draw_target;
    const unsigned int total = fb_width * fb_height;
    for (unsigned int i = 0; i < total; ++i)
    {
        p[i] = c;
    }
}

void fb_pixel(long long x, long long y, long long color)
{
    if (!fb_draw_target)
    {
        return;
    }
    if (x < 0 || y < 0 || x >= fb_width || y >= fb_height)
    {
        return;
    }
    volatile unsigned int* p =
        fb_draw_target + (unsigned int)y * (fb_target_stride / 4) + (unsigned int)x;
    *p = (unsigned int)color;
}

void fb_fill_rect(long long x, long long y, long long w, long long h, long long color)
{
    if (!fb_draw_target)
    {
        return;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }
    // Clip to the framebuffer (defense-in-depth; Curlee geometry is already
    // in-bounds via clip_rect/blit_fits).
    long long x0 = (x < 0) ? 0 : x;
    long long y0 = (y < 0) ? 0 : y;
    long long x1 = (x + w > fb_width) ? fb_width : (x + w);
    long long y1 = (y + h > fb_height) ? fb_height : (y + h);
    const unsigned int c = (unsigned int)color;
    for (long long py = y0; py < y1; ++py)
    {
        volatile unsigned int* row =
            fb_draw_target + (unsigned int)py * (fb_target_stride / 4);
        for (long long px = x0; px < x1; ++px)
        {
            row[(unsigned int)px] = c;
        }
    }
}

// Bresenham line. Bounds-checked per pixel (clipped at the edges).
void fb_line(long long x0, long long y0, long long x1, long long y1, long long color)
{
    if (!fb_addr)
    {
        return;
    }
    const long long dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    const long long dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    const long long sx = (x0 < x1) ? 1 : -1;
    const long long sy = (y0 < y1) ? 1 : -1;
    long long err = dx - dy;
    const unsigned int c = (unsigned int)color;
    while (1)
    {
        fb_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        const long long e2 = 2 * err;
        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

// ---------------------------------------------------------------------------
// Text rendering (5x7 glyphs, scaled)
// ---------------------------------------------------------------------------

// 5x7 glyph row for a character (bit 4 = leftmost pixel). Mirrors the
// glyph tables in glyphs.curlee — keep in sync.
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

void fb_draw_char_color(long long ch, long long x, long long y, long long scale,
                        long long color)
{
    if (!fb_addr)
    {
        return;
    }
    if (scale < 1)
    {
        scale = 1;
    }
    const unsigned int c = (unsigned int)color;
    for (int row = 0; row < 7; ++row)
    {
        unsigned char bits = glyph_row((char)ch, row);
        for (int col = 0; col < 5; ++col)
        {
            // Bit 4 (0x10) = leftmost pixel.
            int on = (bits >> (4 - col)) & 1;
            if (!on)
            {
                continue;
            }
            // Draw a scale x scale block of the requested color.
            for (long long dy = 0; dy < scale; ++dy)
            {
                for (long long dx = 0; dx < scale; ++dx)
                {
                    fb_pixel(x + col * scale + dx, y + row * scale + dy, c);
                }
            }
        }
    }
}

// Phase-1 compatibility wrapper: draws in the classic orange.
void fb_draw_char(long long ch, long long x, long long y, long long scale)
{
    fb_draw_char_color(ch, x, y, scale, 0x00FF8800);
}

// ---------------------------------------------------------------------------
// Asset blitting (32bpp RAW pixel buffer)
// ---------------------------------------------------------------------------

void fb_blit_asset(long long src, long long src_w, long long src_h,
                   long long dst_x, long long dst_y)
{
    if (!fb_draw_target || src == 0)
    {
        return;
    }
    if (src_w <= 0 || src_h <= 0)
    {
        return;
    }
    // Full OOB gate: the whole asset must fit (Curlee's blit_fits already
    // guarantees this; double-check as defense-in-depth).
    if (dst_x < 0 || dst_y < 0 || dst_x + src_w > fb_width || dst_y + src_h > fb_height)
    {
        return;
    }
    const volatile unsigned int* src_p = (const volatile unsigned int*)(unsigned long)src;
    const unsigned int tstride = fb_target_stride / 4;
    for (long long py = 0; py < src_h; ++py)
    {
        volatile unsigned int* dst_row =
            fb_draw_target + (unsigned int)(dst_y + py) * tstride + (unsigned int)dst_x;
        const volatile unsigned int* src_row = src_p + (unsigned long)py * (unsigned long)src_w;
        for (long long px = 0; px < src_w; ++px)
        {
            dst_row[(unsigned int)px] = src_row[(unsigned int)px];
        }
    }
}

// ---------------------------------------------------------------------------
// Present + event loop
// ---------------------------------------------------------------------------

// Phase 2c: fb_present() is now a REAL flip when the frame ring is active.
//
// Lifecycle:
//   1. Before the first present, ring_active == 0 and fb_draw_target IS the
//      visible framebuffer (single-buffer 2a/2e behavior exactly — writes are
//      immediately visible, fb_present is a no-op).
//   2. On the first present, if the framebuffer fits a ring slot (Curlee's
//      frame_ring_fits gates this; here we enforce the same condition with
//      the C constants), the ring activates: ring_active = 1, and the draw
//      target re-points at frame_ring[ring_slot] (the back buffer).
//   3. Every subsequent present copies the CURRENT back buffer onto the
//      visible framebuffer (a full-frame 32bpp memcpy), then advances
//      ring_slot via the verified ring arithmetic (frame_ring_next: 0->1->0)
//      and re-points the target at the new back buffer.
//
// The flip is a copy (not a page flip) because the multiboot2 framebuffer is
// a single linear surface — there is no hardware scanline base to swap. The
// copy is bounded by fb_width * fb_height * 4 (the visible surface size),
// which frame_ring_fits guarantees is <= FRAME_RING_MAX_W * MAX_H * 4, so
// the back buffer is always large enough. No partial writes: the copy only
// happens after render_frame has fully drawn the back buffer (render_frame
// calls fb_present last).
void fb_present(void)
{
    if (!fb_addr)
    {
        return;
    }

#ifdef JOE_PVH_BOOT
    // PVH path: no framebuffer ring (compile-time empty), so present is a
    // no-op — single-buffer 2a/2e behavior exactly.
    (void)ring_active;
    (void)ring_slot;
    (void)fb_target_stride;
    return;
#else
    if (!ring_active)
    {
        // First present: check the framebuffer fits a ring slot (same gate as
        // Curlee's frame_ring_fits). If not, stay single-buffered forever
        // (no-op present — preserves 2a/2e behavior on oversized modes).
        if (fb_width > FRAME_RING_MAX_W || fb_height > FRAME_RING_MAX_H)
        {
            return;
        }
        ring_active = 1;
        ring_slot = 0;
        fb_draw_target = frame_ring[ring_slot];
        fb_target_stride = FRAME_RING_MAX_W * 4;
        return;
    }

    // Ring active: copy the back buffer we just rendered onto the visible
    // framebuffer, then advance the slot (verified ring arithmetic).
    const volatile unsigned int* src =
        (const volatile unsigned int*)fb_draw_target;
    volatile unsigned int* dst = (volatile unsigned int*)fb_addr;
    const unsigned int copy_w = fb_width;              // pixels per row
    const unsigned int copy_h = fb_height;             // rows
    const unsigned int src_stride = fb_target_stride / 4;
    const unsigned int dst_stride = fb_pitch / 4;
    for (unsigned int row = 0; row < copy_h; ++row)
    {
        const volatile unsigned int* s = src + (unsigned long)row * src_stride;
        volatile unsigned int* d = dst + (unsigned long)row * dst_stride;
        for (unsigned int col = 0; col < copy_w; ++col)
        {
            d[col] = s[col];
        }
    }

    // Advance: 0 -> 1 -> 0, matching assets.curlee's frame_ring_next /
    // ring_next exactly for FRAME_RING_SLOTS == 2 (C is free to use %; the
    // division-free rule applies to the Curlee verifier fragment only).
    ring_slot = (ring_slot + 1) % FRAME_RING_SLOTS;
    fb_draw_target = frame_ring[ring_slot];
    fb_target_stride = FRAME_RING_MAX_W * 4;
#endif
}

// ---------------------------------------------------------------------------
// Phase 2b: 60 FPS event loop + kernel tool-call queue
// ---------------------------------------------------------------------------
//
// The C driver owns ALL mutable loop state (the Curlee language has no
// assignment). Curlee's `main` drives the deterministic while-loop — it is
// the one exported symbol the codegen produces (every other function is
// `static`, so fb.c cannot call curlee_render_frame directly). Per frame,
// main calls:
//
//   frame = fb_loop_frame();   // current counter
//   render_frame(pm, frame);   // Curlee scene (frame-aware)
//   serial marker;             // FR:<n> (qemu-loop-smoke gate)
//   fb_run_loop();             // ONE tick: drain the tool ring + advance
//
// The loop is fuel-bounded in Curlee (while fb_loop_frame() < N), so the
// smoke gates stay deterministic and timeout-safe: the kernel renders N
// frames, emits FB: 1, then falls through to the Phase 1 VGA + serial +
// halt path. No malloc, no libc — static arrays only.

// Tool-queue ring geometry. MUST stay in sync with assets.curlee:
//   tool_queue_slots() == 8, tool_slot_bytes() == 16.
// canvas_test.curlee §12 asserts the exact values; a drift fails `make
// canvas-run` before any boot.
#define TOOL_QUEUE_SLOTS 8
#define TOOL_SLOT_BYTES  16

// One fixed tool-call slot: a (kind, arg) intent. The Curlee contract layer
// (tool_queue_slot / tool_slot_offset in kernel.curlee / assets.curlee)
// computes the geometry; this struct is the C layout both sides agree on.
struct tool_slot
{
    long long kind;
    long long arg;
};

// The ring: fixed-size static array (no malloc). A slot is "empty" when its
// kind is 0 (kind 0 is reserved as the null intent).
static struct tool_slot tool_queue[TOOL_QUEUE_SLOTS];

// Ring bookkeeping: the next slot to write (producer cursor) and the number
// of live (not-yet-drained) intents.
static unsigned int tool_head = 0;
static unsigned int tool_count = 0;

// The 60 FPS loop frame counter (the loop's fuel in Curlee main).
static unsigned long long loop_frame = 0;

// Intents drained by the most recent fb_run_loop() tick (observable via
// fb_tool_drained, which the Curlee loop layer can use to prove the ring
// advanced).
static unsigned int loop_drained = 0;

// ---------------------------------------------------------------------------
// Tool-queue producer API (called by Curlee kernel producers)
// ---------------------------------------------------------------------------

// Enqueue a (kind, arg) tool intent into the fixed ring.
// Returns 1 on success, 0 when the ring is full (producers must back off).
// Kind 0 is reserved (null intent) — never enqueue kind 0.
long long fb_tool_enqueue(long long kind, long long arg)
{
    if (kind == 0)
    {
        return 0;
    }
    if (tool_count >= TOOL_QUEUE_SLOTS)
    {
        return 0; // ring full
    }
    tool_queue[tool_head].kind = kind;
    tool_queue[tool_head].arg = arg;
    tool_head = (tool_head + 1) % TOOL_QUEUE_SLOTS;
    ++tool_count;
    return 1;
}

// Number of tool intents currently queued (0..TOOL_QUEUE_SLOTS).
long long fb_tool_pending(void)
{
    return (long long)tool_count;
}

// Number of tool intents drained by the last fb_run_loop() tick.
long long fb_tool_drained(void)
{
    return (long long)loop_drained;
}

// ---------------------------------------------------------------------------
// Loop control (called by Curlee main)
// ---------------------------------------------------------------------------

// Reset the frame counter, the tool ring, and the frame ring (called once by
// Curlee main before entering the loop). void return to match the Curlee
// `-> Unit` extern exactly (codegen emits `extern void fb_loop_init(void)`).
void fb_loop_init(void)
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
    // Reset the Phase 2c frame ring to single-buffer mode. fb_present() will
    // re-activate the ring on the first flip of this loop run.
    ring_active = 0;
    ring_slot = 0;
    fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
    fb_target_stride = fb_pitch;
#endif
}

// Current frame counter (the loop's fuel in Curlee main).
long long fb_loop_frame(void)
{
    return (long long)loop_frame;
}

// ---------------------------------------------------------------------------
// The per-frame tick (called by Curlee main each iteration)
// ---------------------------------------------------------------------------

// One loop tick: consume (drain) any queued tool intents, then advance the
// frame counter. The actual render + present happen in Curlee's render_frame
// (called by main before fb_run_loop; render_frame calls fb_present() last,
// which performs the Phase 2c back-buffer flip); this function is where the
// C driver would poll input and dispatch drained tool intents in a later
// phase.
//
// Deterministic + fuel-bounded by design: it never spins; it advances the
// counter exactly once per call, so the Curlee while-loop is the fuel gate.
void fb_run_loop(void)
{
    // Drain the tool ring: consume all queued intents. The consumer is the
    // loop itself (the render already happened this frame); draining here
    // proves the ring advances and keeps the queue bounded (producers can
    // never outrun the consumer beyond the fixed capacity).
    unsigned int drained = 0;
    while (tool_count > 0)
    {
        // Read the oldest live slot. The queue is a ring with head = next
        // write position; the oldest is at (head - count) mod capacity.
        unsigned int idx = (tool_head + TOOL_QUEUE_SLOTS - tool_count) %
                           TOOL_QUEUE_SLOTS;
        // Consume it (mark empty by clearing the reserved kind 0).
        tool_queue[idx].kind = 0;
        tool_queue[idx].arg = 0;
        --tool_count;
        ++drained;
    }
    loop_drained = drained;

    // Advance the frame counter (the loop fuel).
    ++loop_frame;
}
