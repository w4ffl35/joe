# C Boundary Policy — JOE OS

**Status:** Active policy (enforced by `make c-boundary`)
**Applies to:** every `*.c` / `*.h` file under `kernel/` (and any future driver)
**Goal:** stop the C surface from ballooning; push everything expressible in
Curlee into the pure, verified Curlee layer.

---

## 1. The rule (one sentence)

> **No logic in C — only I/O touches and raw memory moves.**

C exists for exactly two reasons in JOE:

1. **I/O ports** — `outb`/`inb`/`outw`/`inw`/`outl`/`inl` are now Curlee
   compiler builtins (inline x86 in/out asm in the freestanding codegen).
   `putc_driver.c` was the first migration (COM1 driver → `serial.curlee`,
   issue #9); `vga_setup.c` was the second: the 24-write VGA text-mode-3
   register sequence is now a genuine Curlee function (`vga_setup.curlee`,
   issue #10); `vbe.c` was the third: the whole Bochs VBE probe (ports
   0x1CE/0x1CF) is now a genuine Curlee function (`vbe.curlee`, issue #11);
   `virtio_net.c` was the last (issue #14): the entire 798-line VirtIO-net
   driver — PCI config-space probe, legacy virtqueue setup, RX/TX, frame
   I/O — is now `virtio_net.curlee`, with only the PVH-conditional
   ring/buffer memory + the base-address getters + the sfence left in C
   (see §3).
2. **Raw memory moves** — volatile framebuffer writes, ring DMA, and the
   mutable driver *state* that the Curlee layer does not express. The
   sequential 0xB8000 text-buffer clear was a raw memory move that *could
   not* migrate at issue #10's toolchain: Curlee `Phys<T>` addresses must be
   compile-time literals and there was no runtime-address physical **write**
   builtin, so a 2000-cell clear was not expressible as a bounded Curlee loop
   (`vga_text_clear.c`). The runtime-address `phys_write_u*` builtins
   (curlee #285) landed since and gh issue #20 DELETED that file — the clear
   is now a genuine Curlee loop in `vga_setup.curlee`. The framebuffer
   globals the VBE probe fills (`fb_addr/pitch/width/height`) were C-visible
   `.data` state owned by `fb.c`, filled through the `vbe_state.c` /
   `mb2_state.c` setter shims; the toolchain's `static` + `[T; N]` arrays
   (curlee #278) landed and gh issue #20 DELETED those shims — the state is
   Curlee statics in `fb.curlee`, filled by its `fb_state_set` (called by
   `mb2.curlee` / `vbe.curlee`) and read directly by the blitter. The
   1056-line TCP/IP stack (`net_stack.c`) migrated in issue #12: the protocol
   logic became `net_stack.curlee` (pure, VM-verified) + the kernel.curlee
   glue, and the file was reduced to a raw-state shim — the mutable one-shot
   connection state and the 256-byte response byte store. gh issue #20
   DELETED that shim too: the state is Curlee statics in `net_glue.curlee`
   (`[Int; 256]` response store). The framebuffer blitter (`fb.c`, 664
   lines) migrated in issue #13: the pixel primitives, Bresenham, the Phase
   2c ring flip, the tool-call queue and the loop control became `fb.curlee`
   (genuine Curlee, merged into the kernel TU), the SMALL mutable state
   (tool ring, loop counters, ring bookkeeping, draw-target indirection)
   moved into Curlee `static` module state, and the runtime-address memory
   moves became Curlee COMPILER BUILTINS (`phys_write_u32` / `phys_read_u8/
   u32` — inline volatile stores/loads, no C symbol), leaving `fb.c` a
   ~90-line build-geometry shim: the two large PVH-conditional buffers only
   (see §3).

Everything else — parsers, protocol logic, checksums, layout math, glyph
tables, geometry — belongs in the **pure, verified Curlee layer**
(`canvas.curlee`, `glyphs.curlee`, `assets.curlee`, `json.curlee`, ...).

The pattern is already proven: `json.curlee` is a 692-line pure parser that
was deliberately written in Curlee (not C), VM-tested, and codegen-verified.
The network protocol logic in `net_stack.c` followed in issue #12 — the full
ARP/IPv4/TCP byte layout, the RFC 1071/793 checksums and the HTTP framing
state machine are now `net_stack.curlee` (pure, VM-verified against the C
ground truth) plus the kernel.curlee glue. The multiboot2 parser in `mb2.c`
was the *same class of code* and migrated in issue #15 as the language gained
assignment + bitwise ops + the runtime-address `phys_read_u*` reads (curlee
issues #268/#270/#279): the tag walk is now `mb2.curlee`, host-verified
against scripted physical memory (`make mb2-codegen-run`), with only the
raw-state shim (`mb2_state.c`: the info-addr getter, the four global stores,
and the raw volatile reads) left in C (see §3).

## 2. What "no logic" means concretely (review checklist)

A C file **violates** the policy if it contains:

- **Protocol/format logic** — big-endian header packing, checksums, length
  framing, field parsing. (`net_stack.c`'s ARP/IPv4/TCP byte layout was the
  canonical violation — migrated to `net_stack.curlee` in issue #12;
  `mb2.c`'s tag walk was a parser — migrated to `mb2.curlee` in issue #15.)
- **Pure data tables** — glyph bitmaps, lookup tables, color palettes.
  (`fb.c` carried a 5x7 glyph table duplicate until 2026-08 — deleted; the
  authoritative copy is `glyphs.curlee`.)
- **Geometry / layout math** — anything `canvas.curlee` / `assets.curlee`
  could express (rect fits, clipping, ring offsets).
- **Algorithmic loops over pure data** — a `for` loop that could be a Curlee
  recursion (the `json_feed` pattern).

A C file **satisfies** the policy if it is:

- An **extern-implementation shim**: `void fb_pixel(x,y,c) { *(uint32_t*)(fb_draw_target + ...) = c; }`
- A **register-sequence driver**: `outb(0x3C4, 0x00); outb(0x3C5, 0x03); ...`
- A **raw ring/state owner**: mutable counters + static arrays, with the
  *geometry* computed in Curlee and asserted by the VM test.

## 3. Size caps (hard numbers, enforced by `make c-boundary`)

| Cap | Value | Why |
|---|---|---|
| Max lines per `.c` file | **200** | `vga_text_clear.c` (29) proved drivers are small before its deletion (`putc_driver.c`'s 43 and `vga_setup.c`'s 73 lines are gone — ported to `serial.curlee` and `vga_setup.curlee`, issues #9/#10) |
| Max pure-logic lines per `.c` file | **0** | Pure logic belongs in Curlee, period |
| Max new `.c`/`.h` files added per feature | **1** | A new device should be one driver + Curlee modules |

The current C surface under `kernel/` — ALL files below the cap and in the
exempt category (raw memory moves / build geometry / linkage shims), with
zero pure logic and **no grandfathered files**: the last one,
`libgcc32.c` (322 lines, "never migrates — GCC ABI"), was DELETED in gh
issue #21 once curlee #288 bundled the equivalent 32-bit helpers into the
curlee toolchain runtime (`runtime/libgcc32_helpers.c` — the GRUB/ISO path
links that toolchain-owned file now instead of a kernel-local copy). The
remaining files are exactly:

- **`fb.c`** (~90 lines) — the two LARGE PVH-conditional buffers (the
  128x128 asset region + the 2x640x480 frame ring, compiled OUT on the PVH
  build — `JOE_PVH_BOOT`): Curlee has no conditional compilation and both
  builds compile the same merged kernel.c, so a full-size Curlee static
  array would land in the PVH kernel's BSS unconditionally and blow QEMU's
  PVH LOAD budget (docs/phase2c-report.md §4.3). Plus the build-geometry
  externs (`fb_pvh_build` / `fb_asset_region_base_get` /
  `fb_frame_ring_slot_base`, 0 on the PVH stubs). gh issue #20 deleted the
  fb_addr/fb_pitch/fb_width/fb_height globals + the four `fb_*_get`
  getters from this file — the framebuffer state is Curlee statics in
  `fb.curlee`. Migrates when Curlee gains conditional compilation.
- **`virtio_net.c`** (~90 lines) — its 798 lines of driver logic (PCI
  config-space scanning, legacy virtqueue setup, RX/TX ring math, frame I/O)
  migrated to `virtio_net.curlee` in issue #14, leaving the two large
  PVH-conditional buffer groups (5-page qmem + 2x2048 data buffers per
  queue, compiled OUT on the PVH build exactly like `fb.c`'s), the four
  base-address getters (the `fb_asset_region_base_get` pattern), the
  `virtio_net_pvh_build` discriminator, and the `virtio_net_sfence` ring-
  publication fence. The buffers must be C-owned for the PVH-size reason.
  Migrates when Curlee gains conditional compilation.
- **`mb2_state.c`** (~25 lines) — gh issue #20 reduced it to the single
  raw-state half Curlee cannot express: the multiboot2 info pointer.
  `mb2_info_addr` is a WEAK `.data` global that kernel/boot.S (32-bit
  assembly, running BEFORE curlee_main) overwrites with its strong `.data`
  definition on the GRUB path (GNU weak/strong interposition); the Curlee
  window is `mb2_info_addr_get()`. Curlee statics are file-local in the
  codegen (static C symbols), so boot.S cannot write one — a C-level
  linkage requirement, the same class as the sfence. The former
  `mb2_state_set` setter (the four framebuffer-global stores) is DELETED —
  the state is Curlee statics in `fb.curlee` (gh issue #20). Migrates when
  Curlee gains a boot-assembly-visible global.

Also DELETED in gh issue #20 (all three residuals the issue tracked):
- **`vga_text_clear.c`** (29 lines) — the 0xB8000 text-buffer clear is now
  a genuine Curlee loop in `vga_setup.curlee` (runtime-address
  `phys_write_u16`, curlee #285).
- **`vbe_state.c`** (39 lines) — the framebuffer-state setter shim is gone;
  `vbe.curlee` calls `fb.curlee`'s `fb_state_set`.
- **`net_stack.c`** (119 lines) + **`net_stack.h`** — the raw-state shim is
  gone; the one-shot state + 256-byte response store are Curlee statics in
  `net_glue.curlee`.

Also DELETED in gh issue #21 (the compiler-runtime residual, not a driver):
- **`libgcc32.c`** (322 lines) — the 32-bit GCC ABI helpers
  (`__muldi3`/`__udivdi3`/`__umoddi3`/`__udivmoddi4`/`__divdi3`/`__moddi3`/
  `__negdi2`/`__ashldi3`/`__lshrdi3`/`__ashrdi3`/`__cmpdi2`) for the
  `-m32` GRUB path. curlee #288 bundled the equivalent implementation into
  the curlee toolchain runtime (`runtime/libgcc32_helpers.c`), so the GRUB/
  ISO build links the toolchain-owned object — the kernel no longer carries
  its own copy, and `kernel/` has no grandfather list left.

## 4. Migration roadmap (what unblocks what)

The C surface collapses to a thin I/O shim once the Curlee features land
(tracked as GitHub issues in `w4ffl35/curlee`):

1. **Assignment / affine mutation** — unblocked `fb.c`'s blitter loops (the
   pixel primitives, Bresenham, the tool-call queue and the loop control
   migrated to `fb.curlee` in issue #13) and the state machines in
   `virtio_net.c` (migrated in issue #14: every ring loop, the PCI probe
   walk, and the RX/TX state are Curlee assignment + statics). (The `mb2.c`
   tag walk used this — the mutable cursor in `mb2.curlee`, issue #15.)
2. **Port I/O (`outb`/`inb`/`outw`/`inw`/`outl`/`inl`)** — unblocked the
   PCI config half of `virtio_net.c`.
   `putc_driver.c` migrated first on this feature (issue #9, now
   `serial.curlee`); `vga_setup.c`'s register sequence followed (issue #10,
   now `vga_setup.curlee`); `vbe.c`'s Bochs VBE probe followed (issue #11,
   now `vbe.curlee`); `virtio_net.c` followed last (issue #14, now
   `virtio_net.curlee` — the let-bound I/O-BAR base + constant-offset ports,
   curlee #276, cover the whole register set).
   **Runtime-address physical writes** (the `phys_read_u*` counterpart)
   LANDED as Curlee COMPILER BUILTINS and unblocked the `fb.c` blitter in
   issue #13: `phys_write_u32` is now an inline volatile store in the
   freestanding codegen (no C symbol — the `fb.c` shim definition was
   deleted in the 2026-08 revision), so the Curlee pixel primitives write
   the hardware framebuffer at its boot-discovered runtime address. Issue
   #14 uses the full `phys_write_u8/u16/u32/u64` surface to fill the legacy
   vring structures (desc/avail/used) at their runtime 4096-aligned bases.
   gh issue #20 then used the same `phys_write_u16` builtin to delete
   `vga_text_clear.c` — the 0xB8000 text-buffer clear is a genuine Curlee
   loop in `vga_setup.curlee`.
3. **Bitwise ops + shifts** — unblocked big-endian packing, checksums,
   descriptor flags, and alignment math. Landed in the compiler ahead of
   issue #12, which used it to migrate the whole `net_stack.c` protocol core
   (byte layout, RFC 1071/793 checksums, the HTTP framing state machine) to
   `net_stack.curlee`. Issue #15 used it for the `(size + 7) & ~7`
   tag-alignment math in `mb2.curlee`; issue #14 uses it for the PCI config
   address assembly and the `(raw + 4095) & ~4095` queue-base alignment in
   `virtio_net.curlee`.
4. **Runtime-address physical reads (`phys_read_u8/u16/u32/u64`, curlee issue
   #279)** — the piece that unblocked the `mb2.c` tag walk: the multiboot2
   info structure lives at a RUNTIME address captured by the boot stub,
   which a compile-time-literal `Phys<T>` cannot address. `mb2.curlee`
   (issue #15) calls them inside `unsafe` with `cap phys.mem`, exactly like
   the `Phys<T>` reads. In the 2026-08 toolchain these became Curlee
   COMPILER BUILTINS (inline volatile loads — the `mb2_state.c` definitions
   were deleted); their runtime-address physical write counterpart
   (`phys_write_u*`, curlee #285) is the builtin gh issue #20's
   `vga_text_clear.c` deletion uses. Issue #14 uses them for the RX-frame
   byte reads and the used-ring idx/elem reads in `virtio_net.curlee`.
5. **Fixed-size arrays + statics + Int->U64 widening (curlee #278/#277)** —
   landed in the 2026-08 toolchain. `fb.curlee` (issue #13) moved the small
   mutable blitter state into Curlee `static` + `[T; N]` state; issue #14
   does the same for the whole driver state (rx_buf_state/rx_frame_len/
   guest_mac/ring heads). gh issue #20 used the same feature to delete
   `vbe_state.c` and `net_stack.c` — the framebuffer state is Curlee statics
   in `fb.curlee` (filled by `fb_state_set`), and the TCP one-shot state +
   256-byte response store are Curlee statics in `net_glue.curlee`. The
   descriptor `addr` u64 field is built with the `Int -> U64` widening for
   values < 2^32 (curlee #277).

Until then, **do not add new pure logic to C**. If a feature needs pure
logic, write it as a Curlee module (even if the driver can't yet be
refactored) and have the C side consume it via the extern window.

## 5. Enforcement (`make c-boundary`)

`scripts/check-c-boundary.sh` runs on every `make verify` (and standalone):

- **Fails** if any `kernel/*.c` exceeds **200 lines** (no grandfathers
  remain — `libgcc32.c` was the last, deleted in gh issue #21; the 32-bit
  GCC ABI helpers now live in the curlee toolchain runtime).
- **Fails** if any `kernel/*.c` contains a `switch` with >4 cases or a
  `static const` array >32 elements (pure-data red flags).
- **Fails** if any `kernel/*.c` calls `fb_*`-style externs back into Curlee
  (the C layer must never call up).
- **Reports** (warning) line counts so the trend is visible in CI.

The script is deliberately conservative: it catches the *class* of violation
(protocol logic, data tables) without needing a full AST. Human review is
still the primary gate for "is this logic or I/O?".
