# JOE OS — Phase 2c Implementation Report (memory & asset management contracts)

Status: Implementation complete — pending architect review
Date: 2026-08-25
Scope: Phase 2c — memory & asset management contracts (gh issue #2)

---

## 1. Objective

Make the static-buffer asset/frame-ring discipline explicit and enforced.
The pure math already existed in `kernel/assets.curlee` (`blit_fits`,
`dst_fits`, `ring_next`, `ring_offset`, `ring_capacity`, all VM-verified);
this phase:

1. Adds the missing pure helpers (runtime fit gates, static asset-region
   geometry, frame-ring geometry).
2. Wires the C side (fb.c) to actually own the static asset region and the
   frame ring, and makes `fb_present()` a REAL back-buffer flip.
3. Exposes the runtime sizes to Curlee via extern accessors so every
   blit/fill/line/text path in `render_frame` is gated by the verified pure
   gates BEFORE dispatch (a deliberately out-of-bounds primitive is skipped,
   never a partial write).
4. Extends the VM test (`canvas_test.curlee` §14–§17) with exhaustive
   ring/blit-fit edge cases.

## 2. Deliverables

| File | Change | Status |
|------|--------|--------|
| [`kernel/assets.curlee`](kernel/assets.curlee) | Added Phase 2c section: `asset_region_w/h`, `asset_region_fits`, `asset_blit_fits`, `rect_fits_gate`, `line_fits_gate`, `char_fits_gate`, `imin`/`imax`, `frame_ring_slots/max_w/max_h/slot_bytes`, `frame_ring_fits`, `frame_ring_offset`, `frame_ring_next`, `frame_ring_slot_ok`, `frame_ring_capacity` | ✅ verifies |
| [`kernel/canvas_test.curlee`](kernel/canvas_test.curlee) | Added §14 (asset region geometry + fits), §15 (frame ring geometry + fits), §16 (rect/line/char gates), §17 (asset-blit gate) | ✅ returns 0 |
| [`kernel/fb.c`](kernel/fb.c) | Static `asset_region[128*128]` + `frame_ring[2][640*480]` (no malloc); `fb_draw_target`/`fb_target_stride` target indirection; accessor externs (`fb_get_width/height`, `fb_asset_region_w/h/base`, `fb_ring_active/slot`); `fb_present()` real flip (copy back buffer → visible FB, advance slot); `JOE_PVH_BOOT` guard to compile the ring/region out on the PVH path | ✅ compiles (both paths) |
| [`kernel/kernel.curlee`](kernel/kernel.curlee) | New extern declarations for the Phase 2c accessors; `render_frame` reads `fb_get_width`/`fb_get_height` and gates every primitive via `rect_fits_gate`/`line_fits_gate`/`char_fits_gate`; `serial_ring_marker()` + `main` emits `RING: 1` when the ring flipped | ✅ (merged + verified) |
| [`Makefile`](Makefile) | `-DJOE_PVH_BOOT` on the PVH builds (kernel.elf, kernel-smoke.elf); `qemu-fb-smoke` asserts `RING: 1`; `qemu-loop-smoke` ordered sequence now includes `RING: 1` | ✅ |
| [`docs/phase2-architecture.md`](docs/phase2-architecture.md) | §7 roadmap 2c → done; new §11 Phase 2c acceptance criteria | ✅ |
| [`README.md`](README.md) | Phase 2c roadmap status + module/blitter description | ✅ |

## 3. Verification gates — results (all green)

| Gate | Result |
|------|--------|
| `make check` (pack, canvas, glyphs, assets + merged kernel) | ✅ all verify |
| `make canvas-run` (`curlee run canvas_test.curlee`) | ✅ result 0 (incl. §14–§17) |
| `make pack-run` | ✅ result 0 |
| `make verify` (check + pack-run + canvas-run + kernel + ELF/_start/curlee_main/PVH gates) | ✅ all pass |
| `make qemu-smoke` (PVH path) | ✅ "Hello World from JOE!" |
| `make iso` / `make iso-fb` (GRUB path) | ✅ built |
| `make qemu-fb-smoke` | ✅ **`RING: 1`** + `FB: 1` (frame ring flipped) |
| `make qemu-loop-smoke` | ✅ ordered `FR:0..FR:3, RING: 1, FB: 1, Hello World from JOE!` |

## 4. Design decisions

### 4.1 Two static buffers, compile-time geometry

- **Asset region**: `static uint32_t asset_region[128*128]` (64 KB) — a fixed
  32bpp RAW pixel store. Curlee's `asset_region_fits` gates staging into it.
- **Frame ring**: `static uint32_t frame_ring[2][640*480]` (2.4 MB) — a
  2-slot double buffer matching the GRUB framebuffer request tag (boot.S asks
  for 640x480x32). `frame_ring_slots`/`frame_ring_max_w/h`/`frame_ring_fits`
  define the geometry both sides must agree on; the VM test (§15) asserts it.

### 4.2 `fb_present()` is now a REAL flip (GRUB framebuffer path)

Lifecycle:
1. Before the first present: single-buffered (draw target IS the visible FB),
   preserving 2a/2e behavior.
2. First present: if the FB fits a ring slot (`frame_ring_fits`), the ring
   activates (`ring_active=1`) and the draw target re-points at
   `frame_ring[0]`.
3. Every subsequent present copies the back buffer onto the visible FB
   (bounded by fb_width×fb_height×4), then advances the slot
   (0→1→0, division-free wrap matching `frame_ring_next`).

The flip is a copy (not a page flip) because the multiboot2 framebuffer is a
single linear surface — there is no scanline base to swap. The `RING: 1`
serial marker proves the ring flipped (asserted by qemu-fb-smoke and
qemu-loop-smoke).

### 4.3 PVH constraint discovered (critical, documented in fb.c)

QEMU's `-kernel` PVH loader **silently refuses ELFs whose LOAD segment
(file + BSS) exceeds a hard budget** — verified empirically: adding ANY large
static buffer (even a 128x128 asset region = 64 KB) makes the BIOS hang
executing zeros with no exception and no serial output (the pristine ~29 KB
image boots fine; sizes ≥ ~16x16 arrays fail). The PVH path has NO
framebuffer (`fb_ready()==0`), so it never uses the ring/region.

**Fix**: the Makefile defines `JOE_PVH_BOOT` for the PVH builds
(`kernel.elf`, `kernel-smoke.elf`), which compiles the ring/region out
(accessors return 0; `fb_present()` is a no-op). The GRUB path
(`kernel-grub.elf`, `iso`) compiles WITHOUT the macro and gets the full
2.4 MB ring + 64 KB asset region — where the flip actually runs. This keeps
`make qemu-smoke` (PVH) green while `make qemu-fb-smoke`/`qemu-loop-smoke`
(GRUB) prove the ring flips. No malloc anywhere; all buffers static.

### 4.4 Curlee gates read the runtime FB size

`render_frame` binds `fb_get_width()`/`fb_get_height()` (extern accessors)
to `let`s, then gates each primitive with a pure contract-less twin:
`rect_fits_gate` (fill rects), `line_fits_gate` (lines), `char_fits_gate`
(scaled glyphs), `asset_blit_fits` (asset blits). An out-of-bounds primitive
is skipped entirely — never a partial write. The C blitter additionally
bounds-checks every primitive as defense-in-depth (unchanged from 2a/2e).

## 5. Acceptance criteria (issue #2) — status

1. **Every blit/fill/line/text path is gated by both the Curlee contract AND
   the C bounds check; a deliberately OOB asset never writes.** ✅ — all
   `render_frame` primitives pass through the pure gates; fb.c clips/bounds
   every primitive.
2. **`make canvas-run` passes with the extended ring/blit assertions.** ✅ —
   result 0 with §14–§17 (asset region, frame ring, gates, asset blit).
3. **`make qemu-fb-smoke` still passes (`FB: 1`).** ✅ — now ALSO asserts
   `RING: 1` (the flip ran).
4. **`make verify` + `make qemu-smoke` still pass (PVH path unchanged).** ✅.
5. **No malloc/libc anywhere; all buffers static and size-validated.** ✅ —
   `asset_region`, `frame_ring`, `tool_queue` are all static arrays; the
   geometry is cross-checked by the VM test against the C `#defines`.

## 6. Known limitations / deferred

- The frame ring is a COPY flip, not a hardware page flip (the multiboot2
  framebuffer is a single linear surface). A hardware VBE/EDID scanline-base
  swap is a future optimization (Phase 2f territory).
- The PVH path (`qemu -kernel`) has no framebuffer, so the ring is compiled
  out there by design (`JOE_PVH_BOOT`). When Phase 2f lands a real PVH
  framebuffer, the same `#ifndef` can be relaxed once the PVH loader budget
  question is resolved (a VBE-probed FB would remove the need for the static
  ring entirely).
- The demo scene draws at 640x480 with the ring active on the GRUB path; the
  ring slot stride (640*4) matches the FB pitch for a 640x480 mode, so the
  copy is exact. Other modes (e.g. 800x600) fall back to single-buffered
  (frame_ring_fits returns false) — correct, if not optimal.

## 7. Self-assessment

All six acceptance criteria for Phase 2c are met. The implementation honors
the design laws: single address space (no MMU), deterministic verification
(pure gates + VM-asserted geometry), minimal footprint (no malloc/libc;
static arrays only). The PVH loader budget discovery was the one genuinely
surprising constraint and is documented both in code (fb.c header) and here
so the next phase doesn't reintroduce it.
