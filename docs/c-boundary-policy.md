# C Boundary Policy — JOE OS

**Status:** Active policy (enforced by `make c-boundary`)
**Applies to:** every `*.c` / `*.h` file under `kernel/` (and any future driver)
**Goal:** stop the C surface from ballooning; push everything expressible in
Curlee into the pure, verified Curlee layer.

---

## 1. The rule (one sentence)

> **No logic in C — only I/O touches and raw memory moves.**

C exists for exactly two reasons in JOE:

1. **I/O ports** — `outb`/`inb`/`outw`/`inw`/`outl`/`inl` and the inline-asm
   wrappers (`virtio_net.c`). `putc_driver.c` was the first migration (COM1
   driver → `serial.curlee`, issue #9); `vga_setup.c` was the second: the
   24-write VGA text-mode-3 register sequence is now a genuine Curlee
   function (`vga_setup.curlee`, issue #10); `vbe.c` was the third: the
   whole Bochs VBE probe (ports 0x1CE/0x1CF) is now a genuine Curlee
   function (`vbe.curlee`, issue #11).
2. **Raw memory moves** — volatile framebuffer writes, ring DMA, and the
   mutable driver *state* (frame counter, ring indices) that the Curlee layer
   does not express yet (assignment exists — issue #268 — but these drivers
   are not yet migrated). The 0xB8000 text-buffer clear is a raw memory move
   that *cannot* migrate: Curlee `Phys<T>` addresses must be compile-time
   literals and there is no runtime-address physical **write** builtin (only
   the `phys_read_u*` reads of curlee issue #279), so a sequential
   2000-cell clear is not expressible as a bounded Curlee loop
   (`vga_text_clear.c` stays in C for this reason). The framebuffer globals
   the VBE probe fills (`fb_addr/pitch/width/height`) are C-visible `.data`
   state owned by `fb.c`, and Curlee has no globals, so those four stores
   stay in C as a raw state shim (`vbe_state.c`, issue #11) — same class as
   `vga_text_clear.c`'s memory move. The 1056-line TCP/IP stack
   (`net_stack.c`) migrated in issue #12: the protocol logic became
   `net_stack.curlee` (pure, VM-verified) + the kernel.curlee glue, and the
   file is now a raw-state shim in the same class — the mutable one-shot
   connection state and the 256-byte response byte store Curlee cannot own
   (no globals, no arrays), with no protocol logic left.

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
| Max lines per `.c` file | **200** | `vga_text_clear.c` (35) proves drivers are small (`putc_driver.c`'s 43 and `vga_setup.c`'s 73 lines are gone — ported to `serial.curlee` and `vga_setup.curlee`, issues #9/#10) |
| Max pure-logic lines per `.c` file | **0** | Pure logic belongs in Curlee, period |
| Max new `.c`/`.h` files added per feature | **1** | A new device should be one driver + Curlee modules |

The current offenders (grandfathered, tracked for migration):

| File | Lines | Pure-logic estimate | Migrate when |
|---|---|---|---|
| `virtio_net.c` | 798 | ~300 (ring math) | Curlee gains assignment + port I/O |
| `fb.c` | 664 | ~150 (Bresenham, loops) | Curlee gains assignment |
| `libgcc32.c` | 322 | 0 (compiler shim) | never (GCC ABI) |

(`net_stack.c` is gone from this table: its ~700 lines of protocol logic
migrated to `net_stack.curlee` + the kernel.curlee glue in issue #12, and
the file is now a ~110-line raw-state shim in the exempt category above.
`mb2.c` is gone too: its 116-line tag walk migrated to `mb2.curlee` in issue
#15, leaving a ~50-line raw-state shim (`mb2_state.c` — the info-addr
getter, the four framebuffer-global stores, and the runtime-address
`phys_read_u8/u32` raw volatile loads, curlee issue #279) in the exempt
category above.)

## 4. Migration roadmap (what unblocks what)

The C surface collapses to a thin I/O shim once three Curlee features land
(tracked as GitHub issues in `w4ffl35/curlee`):

1. **Assignment / affine mutation** — unblocks `fb.c` (blitter loops),
   the state machines in `virtio_net.c`. (The `mb2.c` tag walk used this —
   the mutable cursor in `mb2.curlee`, issue #15.)
2. **Port I/O (`outb`/`inb`/`outw`/`inw`/`outl`/`inl`)** — unblocks the
   PCI config half of `virtio_net.c`.
   `putc_driver.c` migrated first on this feature (issue #9, now
   `serial.curlee`); `vga_setup.c`'s register sequence followed (issue #10,
   now `vga_setup.curlee`); `vbe.c`'s Bochs VBE probe followed (issue #11,
   now `vbe.curlee` — its only C residual is the `vbe_state_set` state shim,
   the globals-write half of the raw-state category above). A
   **runtime-address physical write** (the `phys_read_u*` counterpart) would
   additionally unblock the `vga_text_clear.c` raw memory move.
3. **Bitwise ops + shifts** — unblocked big-endian packing, checksums,
   descriptor flags, and alignment math. Landed in the compiler ahead of
   issue #12, which used it to migrate the whole `net_stack.c` protocol core
   (byte layout, RFC 1071/793 checksums, the HTTP framing state machine) to
   `net_stack.curlee` — only the raw-state shim remains in C. Issue #15 used
   it for the `(size + 7) & ~7` tag-alignment math in `mb2.curlee`.
4. **Runtime-address physical reads (`phys_read_u8/u16/u32/u64`, curlee issue
   #279)** — the last piece that unblocked the `mb2.c` tag walk: the
   multiboot2 info structure lives at a RUNTIME address captured by the boot
   stub, which a compile-time-literal `Phys<T>` cannot address. `mb2.curlee`
   (issue #15) calls them inside `unsafe` with `cap phys.mem`, exactly like
   the `Phys<T>` reads; the definitions are raw volatile loads in
   `mb2_state.c` (the current toolchain runtime does not carry the symbols).
   A **runtime-address physical write** counterpart would additionally unblock
   the `vga_text_clear.c` raw memory move.

Until then, **do not add new pure logic to C**. If a feature needs pure
logic, write it as a Curlee module (even if the driver can't yet be
refactored) and have the C side consume it via the extern window.

## 5. Enforcement (`make c-boundary`)

`scripts/check-c-boundary.sh` runs on every `make verify` (and standalone):

- **Fails** if any `kernel/*.c` exceeds **200 lines** (grandfathered files
  listed in §3 are exempt until migration).
- **Fails** if any `kernel/*.c` contains a `switch` with >4 cases or a
  `static const` array >32 elements (pure-data red flags).
- **Fails** if any `kernel/*.c` calls `fb_*`-style externs back into Curlee
  (the C layer must never call up).
- **Reports** (warning) line counts so the trend is visible in CI.

The script is deliberately conservative: it catches the *class* of violation
(protocol logic, data tables) without needing a full AST. Human review is
still the primary gate for "is this logic or I/O?".
