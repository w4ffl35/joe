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
   wrappers (`vga_setup.c`, `vbe.c`, `virtio_net.c`). `putc_driver.c` was the
   first migration: the COM1 driver is now Curlee (`serial.curlee`, issue #9).
2. **Raw memory moves** — volatile framebuffer writes, ring DMA, and the
   mutable driver *state* (frame counter, ring indices) that the Curlee layer
   does not express yet (assignment exists — issue #268 — but these drivers
   are not yet migrated).

Everything else — parsers, protocol logic, checksums, layout math, glyph
tables, geometry — belongs in the **pure, verified Curlee layer**
(`canvas.curlee`, `glyphs.curlee`, `assets.curlee`, `json.curlee`, ...).

The pattern is already proven: `json.curlee` is a 692-line pure parser that
was deliberately written in Curlee (not C), VM-tested, and codegen-verified.
The network protocol logic in `net_stack.c` and the multiboot2 parser in
`mb2.c` are the *same class of code* and should migrate to Curlee as the
language gains assignment + bitwise ops (see §4).

## 2. What "no logic" means concretely (review checklist)

A C file **violates** the policy if it contains:

- **Protocol/format logic** — big-endian header packing, checksums, length
  framing, field parsing. (`net_stack.c`'s ARP/IPv4/TCP byte layout is the
  canonical violation; `mb2.c`'s tag walk is a parser.)
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
| Max lines per `.c` file | **200** | `vga_setup.c` (73) proves drivers are small (`putc_driver.c`'s 43 lines are gone — ported to `serial.curlee`, issue #9) |
| Max pure-logic lines per `.c` file | **0** | Pure logic belongs in Curlee, period |
| Max new `.c`/`.h` files added per feature | **1** | A new device should be one driver + Curlee modules |

The current offenders (grandfathered, tracked for migration):

| File | Lines | Pure-logic estimate | Migrate when |
|---|---|---|---|
| `net_stack.c` | 1056 | ~700 (protocol) | Curlee gains assignment + bitwise |
| `virtio_net.c` | 798 | ~300 (ring math) | Curlee gains assignment + port I/O |
| `fb.c` | ~680 | ~150 (Bresenham, loops) | Curlee gains assignment |
| `mb2.c` | 116 | ~80 (tag walk) | Curlee gains assignment + bitwise |
| `libgcc32.c` | 322 | 0 (compiler shim) | never (GCC ABI) |

## 4. Migration roadmap (what unblocks what)

The C surface collapses to a thin I/O shim once three Curlee features land
(tracked as GitHub issues in `w4ffl35/curlee`):

1. **Assignment / affine mutation** — unblocks `fb.c` (blitter loops),
   `mb2.c` (tag walk), the state machines in `net_stack.c`/`virtio_net.c`.
2. **Port I/O (`outb`/`inb`/`outw`/`inw`/`outl`/`inl`)** — unblocks
   `vga_setup.c`, `vbe.c`, and the PCI config half of `virtio_net.c`.
   `putc_driver.c` was the first to migrate on this feature (issue #9, now
   `serial.curlee`).
3. **Bitwise ops + shifts** — unblocks big-endian packing in `net_stack.c`,
   checksums, descriptor flags, and alignment math in `mb2.c`.

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
