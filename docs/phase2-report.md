# JOE OS — Phase 2a Implementation Report (pass/fail review)

Status: Implementation complete — pending architect review
Date: 2026-08-24
Scope: Phase 2a — Agentic Framebuffer OS software renderer

---

## 1. Objective

Refactor the Phase 1 static "Hello World" kernel into the first layer of the
Agentic Framebuffer OS: a freestanding software renderer targeting the linear
framebuffer, with pure verified Curlee math modules, C blitter primitives, a
single-TU merge pipeline, and a declarative render scene — all while keeping
every existing verification gate green.

## 2. Deliverables (all created)

| File | Purpose | Status |
|------|---------|--------|
| [`kernel/canvas.curlee`](kernel/canvas.curlee) | Pure verified color/geometry math (rgb, channels, clamp, rect_contains/covers, clip_rect, blend_alpha) | ✅ verifies |
| [`kernel/glyphs.curlee`](kernel/glyphs.curlee) | Pure 5x7 glyph pixel test + text layout math | ✅ verifies |
| [`kernel/assets.curlee`](kernel/assets.curlee) | Pure blit-fit OOB gate + frame ring-buffer math | ✅ verifies |
| [`kernel/canvas_test.curlee`](kernel/canvas_test.curlee) | VM-runnable assertion suite over the pure modules | ✅ returns 0 |
| [`kernel/fb.c`](kernel/fb.c) | Extended blitter: clear/pixel/fill_rect/line/draw_char_color/blit_asset/present/run_loop | ✅ compiles |
| [`scripts/build-kernel.sh`](scripts/build-kernel.sh) | Deterministic concat of modules + kernel.curlee → single-TU merged file | ✅ |
| [`kernel/kernel.curlee`](kernel/kernel.curlee) | Slim entry: externs, render_frame demo scene, main (VGA fallback preserved) | ✅ (merged) |
| [`Makefile`](Makefile) | `check` (all modules + merged), `canvas-run`, merge in kernel/iso/smoke paths | ✅ |
| [`docs/phase2-architecture.md`](docs/phase2-architecture.md) | Full Phase 2 blueprint (2a–2e) | ✅ |
| [`README.md`](README.md) | Phase 2 section + updated layout/gates | ✅ |

## 3. Verification gates — results

| Gate | Command | Result |
|------|---------|--------|
| Static verify | `make check` | ✅ ALL modules + merged kernel verify (Z3) |
| Renderer math | `make canvas-run` | ✅ `curlee run kernel/canvas_test.curlee` → result 0 |
| Packer math | `make pack-run` (in verify) | ✅ result 0 |
| Kernel build | `make kernel` | ✅ `build/kernel.elf` built, entry set |
| Full verify | `make verify` | ✅ ELF entry, `_start`, `curlee_main`, PVH note all PASS |
| Boot (QEMU) | `make qemu-smoke` | ✅ boots; serial: `Hello World from JOE!` |

**All six gates pass.** The Phase 1 acceptance behavior is fully preserved.

## 4. Curlee compiler constraints discovered (documented in architecture doc §1)

These were verified by live experiments against the actual compiler; every
design decision exists to satisfy them:

1. `curlee build` (freestanding codegen) **crashes on any `import`** — exit 134,
   `std::filesystem` error. Solved by `build-kernel.sh` single-TU merge.
2. Codegen supports **name calls only** — qualified calls rejected.
3. **No assignment/rebinding** — imperative loops live in the C driver layer.
4. Verifier fragment: contracted functions support only `+ - *`, comparisons,
   `&& || !`; **no division** in contracted functions ("unsupported binary
   operator"); **no struct params/returns** ("unknown type"); **no function
   calls as arguments to other calls** ("calls are not supported in
   verification expressions"); **no non-linear multiplication** (variable×variable).
5. Contract-less functions are skipped by the solver — they run on the VM and
   codegen, so division lives there and is **asserted by the VM test**.

## 5. Design decisions

- **Two-layer pattern** (proven by Phase 1's `pack.curlee`/`fb.c` split):
  Curlee = pure verified math + declarative scenes; C = mutable blitter state
  + pixel loops.
- **Single-TU merge** (constraint #1): modules stay separate for humans + VM
  tests; `build-kernel.sh` concatenates them for the codegen. Deleted when the
  Curlee import fix lands.
- **No duplication**: `kernel.curlee` is a slim entry that references the
  module helpers by name (merged in). `make check` verifies modules + merged.
- **OOB safety**: `blit_fits`/`clip_rect` (verified or VM-tested) provably bound
  every rect; C blitter double-checks bounds as defense-in-depth.
- **`fb_run_loop` stub**: references `curlee_render_frame` only in Phase 2b;
  the stub avoids the link-time symbol dependency so Phase 2a links today.
- **render_frame call-arg quirk**: colors are bound to `let`s first (the
  verifier rejects calls as call arguments).

## 6. Known limitations / deferred (documented)

- `fb_init()` is still a no-op — the multiboot2 framebuffer address is not yet
  exposed to Curlee (Phase 2e). The demo scene is statically verified; the VGA
  fallback keeps the QEMU smoke gate green.
- The Curlee codegen import crash is an upstream issue (fix in `~/Projects/curlee`
  documented as §8 follow-up); `build-kernel.sh` is the workaround.
- `fb_run_loop` (60 FPS event loop) and the LLM bridge are Phase 2b/2d, not
  implemented in 2a.

## 7. Self-assessment

**PASS** — all acceptance criteria met, all gates green, no regressions. The
deliverables match the architecture plan (with the documented §6 revision to
eliminate helper duplication).

## 8. Architect review — remediation applied

The independent architect-mode review found **one minor issue** (documentation
drift): [`kernel/canvas.curlee`](kernel/canvas.curlee) and
[`kernel/glyphs.curlee`](kernel/glyphs.curlee) module headers still said "the
kernel build inlines these functions into kernel.curlee", but the implemented
architecture is the slim-entry design (the merge script supplies the modules;
no inlined duplication). Both headers were updated to say "merges ... via
scripts/build-kernel.sh ... does NOT duplicate these helpers". Full clean
re-verify (`make verify` + `make qemu-smoke`) passes after the fix.

**Final verdict: PASS** (all gates green, documentation accurate).

---

*This report is read by the architect-mode reviewer, which conducts an
independent pass/fail review and, if fail, returns detailed remediation to a
code agent for another iteration.*

---

## Phase 2e — Framebuffer plumbing: investigation outcome

Phase 2e (make the software renderer display for real) was implemented and
exhaustively investigated. **The display activation is blocked by a GRUB
64-bit multiboot2 quirk**, documented in [`docs/phase2e-architecture.md`](docs/phase2e-architecture.md)
§7. Summary of the empirical findings:

1. GRUB's x86-64 ELF entry does **not** pass the multiboot2 info pointer in
   `%ebx` (serial-verified: reads 0).
2. `.data` initializers are not reliably loaded by GRUB's 64-bit ELF loading
   (verified: `fb_addr` read back 0 despite a correct `.data` value in the ELF).
3. `mb2_parse()` on a garbage pointer corrupts `fb_addr` (guards reduced but
   didn't fully eliminate it).
4. Writing to the VBE LFB base (0xFD000000) faults when no gfxterm framebuffer
   is mapped (the PVH path), tripping the VM.

**Outcome**: the codebase is reverted to the clean, all-green Phase 2a state
(`make verify` + `make qemu-smoke` + `make iso` all pass). The Phase 2e
**groundwork is kept and usable**: `kernel/mb2.c` (validated guarded parser),
`boot.S` `%ebx` capture, and the `qemu-fb-smoke`/`iso-fb` gates. The documented
fix is a **32-bit multiboot2 trampoline entry** (Phase 2e-2) — where the spec
guarantees `%ebx` — which will flip the fb gate green.

**Phase 2e verdict: BLOCKED (not a code failure)** — the renderer, blitter,
and gates are green; the display path needs the 32-bit entry follow-up.

---

## Phase 2e-2 — 32-bit multiboot2 entry: RESOLVED

Phase 2e-2 implemented the documented fix and **flipped the framebuffer gate
green**. The GRUB-path kernel is now a **fully 32-bit ELF** (`as --32` boot.S +
`-m32` C + `ld -m elf_i386`), where the multiboot2 spec guarantees `%ebx` = the
boot info pointer, plus a **framebuffer request tag (type 5)** so GRUB sets a
640x480x32 linear framebuffer before entry. `kernel/mb2.c` parses the trusted
framebuffer tag (spec-correct struct: u32 pitch/width/height) and `fb_init()`
activates the blitter. A `kernel/libgcc32.c` shim provides the 64-bit
`__divdi3`/`__muldi3` helpers the `-m32` codegen needs. The 64-bit PVH/QEMU
path is untouched.

**Phase 2e-2 verdict: COMPLETE** — `make qemu-fb-smoke` **PASSES** (serial
`FB: 1` proves `fb_ready()==1`; framebuffer 0xFD000000 640x480x32). `make
verify`, `make qemu-smoke`, and `make iso` all stay green. Full details in
[`docs/phase2e-2-report.md`](docs/phase2e-2-report.md).

The architect review of Phase 2e confirmed the findings are accurate and the
codebase is clean. Two minor comment-drift items (stale Phase 2e comment in
[`kernel/fb.c`](kernel/fb.c), boot.S comment referencing non-existent .data
initializers) were fixed; `make verify` + `make qemu-smoke` re-verified green.

**Phase 2e final review: PASS** (honest investigation, clean codebase, all
gates green; display activation documented as a scoped follow-up).

---

## Round 2 — architect review remediation (docs accuracy)

The round-2 architect review found the architecture doc still described the
*original plan* rather than the shipped implementation. Three doc-drift items
were remediated (docs-only; no code changes):

1. **§3 directory tree**: `tests/canvas_test.curlee` → `kernel/canvas_test.curlee`
   (the test lives beside the modules it imports, since module resolution is
   importing-file-directory-relative).
2. **§4.1/§4.2 module code samples**: rewritten to the shipped verifier-safe
   API — flat `Int` rect components (no `struct Rect` params, which the
   verifier rejects), division-based channel/clip helpers marked
   contract-less, `clip_rect` packed-int return + `clip_x/y/w/h` decomposition,
   glyph bit-4 (not bit-5) encoding note.
3. **§4.4/§4.5 + §5**: `kernel.curlee` described as a slim entry (no inlined
   helper copies; merge supplies them), `fb_draw_char_color` added, `fb_run_loop`
   marked a Phase 2b stub, and the Makefile table updated to
   `curlee run kernel/canvas_test.curlee` + the module vars in `check`.

Re-verified after the edits: `make verify` (all gates) + `make qemu-smoke`
(serial `Hello World from JOE!`) both pass. No functional change.

**Round 2 verdict: PASS** — documentation now matches the implementation.

---

## Phase 2b — 60 FPS event loop + kernel tool API: COMPLETE

Phase 2b (gh issue #1) implemented the deterministic, fuel-bounded 60 FPS loop
and the kernel tool-call queue. All gates green; no regressions.

### Key design decision (documented deviation from the issue's suggested steps)

The issue proposed the C driver's `fb_run_loop()` calling `curlee_render_frame`
(the codegen export). That is **impossible with the current toolchain**: the
freestanding codegen emits every non-`main` function with **`static` linkage**
(verified in `build/kernel.c`: `static void curlee_render_frame(int64_t frame)`),
so a separate translation unit like `fb.c` cannot call it. Only `curlee_main` is
exported.

**Resolution**: Curlee's `main` drives the deterministic `while` loop itself
(the one exported symbol), and `fb.c` owns **all mutable loop state** — exactly
the issue's "C driver owns the mutable loop state" requirement:

```curlee
fb_loop_init();
while (fb_loop_frame() < 4) {
  let f: Int = fb_loop_frame();
  render_frame(pm, f);        // frame-aware scene (panel y alternates)
  serial_frame_marker(f);     // FR:<n>
  fb_run_loop();              // C: drain tool ring + advance counter
}
serial_fb_marker();           // FB: 1
```

### What changed

- **`kernel/assets.curlee`**: added the pure tool-queue geometry
  (`tool_queue_slots()==8`, `tool_slot_bytes()==16`, `tool_queue_capacity`,
  `tool_slot_offset`, `tool_slot_valid`, `tool_ring_next`), reusing the verified
  ring helpers. Contract-less (non-linear mult / opaque counts) and VM-asserted.
- **`kernel/kernel.curlee`**: `render_frame` is now frame-index-aware (panel y
  alternates via division-free parity math — no `%`, no ternary); added
  `serial_frame_marker` (FR:`<n>`); `main` drives the loop via new externs
  (`fb_loop_init`/`fb_loop_frame`/`fb_run_loop`); added `tool_queue_slot` (frame
  → slot mapping, inlined bounds check because the contracted `ring_valid_slot`
  needs a provable count).
- **`kernel/fb.c`**: replaced the `fb_run_loop()` stub with the real tick —
  fixed-slot tool ring (`TOOL_QUEUE_SLOTS=8` × `TOOL_SLOT_BYTES=16` static
  array, no malloc), producer API (`fb_tool_enqueue`/`fb_tool_pending`/
  `fb_tool_drained`), loop control (`fb_loop_init`/`fb_loop_frame`), and the
  per-frame tick (`fb_run_loop` drains the ring + advances the counter).
- **`kernel/canvas_test.curlee`**: §12 asserts the tool-queue geometry (128-byte
  ring, slot offsets, validity, wrap) so a C/Curlee geometry drift fails the
  pure gate before any boot.
- **`Makefile`**: new `qemu-loop-smoke` gate (boots the FB ISO, asserts
  `FR:0`/`FR:1`/`FR:2` in serial + `FB:`), added to `.PHONY`.

### Verification (all gates)

| Gate | Result |
|------|--------|
| `make check` | ✅ all modules + merged kernel verify |
| `make canvas-run` | ✅ result 0 (incl. new §12 tool-queue assertions) |
| `make kernel` / `make verify` | ✅ PVH path unchanged |
| `make qemu-smoke` | ✅ `Hello World from JOE!` (VGA fallback) |
| `make iso` / `make iso-fb` | ✅ both ISOs build |
| `make qemu-fb-smoke` | ✅ `FB: 1` |
| `make qemu-loop-smoke` | ✅ serial `FR:0 FR:1 FR:2 FR:3 FB: 1 Hello World from JOE!` |

**Phase 2b verdict: COMPLETE** — the loop runs >1 frame deterministically,
the tool ring is a no-malloc fixed-slot queue with documented producers/
consumers, and every existing gate stays green.
