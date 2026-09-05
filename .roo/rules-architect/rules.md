# Architect-mode rules — JOE OS (small-slice decomposition)

Mode-specific supplement for the **architect** (planning) phase. These rules
override the general planning guidance wherever they conflict. The architect
runs on a **9B model with strong single-step tool-use but limited
long-horizon execution** — the entire plan must be built around that
constraint.

## The core rule: plan the work as SMALL, VERIFIABLE SLICES

A code worker session on this model can reliably execute **one small,
self-contained change per session** — a single function, a single build-wiring
edit, a single Makefile target. It loses coherence on anything larger. Your
plan must therefore decompose the goal into the **smallest slices that each
produce a verifiable artifact on their own**, so a worker can land them one
per session with a real check between each.

## Slice-sizing rules (hard)

1. **Each slice = ONE file change or ONE build/gate change**, never both and
   never more. If a slice touches two files, split it.
2. **Each slice must be independently verifiable** with a single command the
   worker can actually run and see pass (e.g. `curlee check <file>`, `make
   check`, a one-line smoke). If a slice can't be verified alone, it is too
   big — split it.
3. **A slice must fit in ~30-80 lines of new code.** If the target file is a
   full driver, do NOT ask a worker to write it whole. Instead emit slices
   like: "slice 3: add the pure `virtio_blk_cell()`/`sector_to_lba()` helper
   functions (pure Int math, verifiable with `curlee check` on the file)".
4. **Dependency order must be explicit**: every slice lists the slices it
   depends on, so workers can run in sequence and each starts from a state
   the previous slice verified.
5. **No slice may require reading more than one reference file.** The 9B
   worker's context is small; pointing it at `kernel/virtio_net.curlee`
   once is fine, asking it to cross-reference three files is not.
6. **Every slice ends with a concrete, checkable definition of done** — a
   file exists AND `make check` passes AND (where relevant) a serial marker
   or gate output was seen. "Implement the driver" is never a slice;
   "add `fn virtio_blk_probe` that returns 0/1 and wire it into
   `kernel.curlee`'s bringup" is.

## Plan format (write PLAN.md in exactly this shape)

```markdown
# Implementation plan: <goal>

## Slice 1 — <name>
- File(s): <one file>
- Change: <what, in 1-3 sentences, concrete>
- Verify: <exact command that proves it, e.g. `curlee check kernel/x.curlee`>
- Depends on: (none)

## Slice 2 — <name>
- File(s): <one file>
- Change: ...
- Verify: ...
- Depends on: Slice 1
...
```

Keep each slice's description short enough that the code worker can hold the
whole slice (its file + the change + the verify command) in one small
context window.

## What NOT to do

- Do NOT write one monolithic "implement the virtio-blk driver" plan — the
  worker cannot execute it and will stall or fabricate.
- Do NOT plan slices that require the worker to hold a large reference
  driver in memory while also writing a large new file.
- Do NOT put multiple responsibilities (PCI probe + virtqueue + read path +
  build wiring + smoke gate) in one slice.
- Do NOT make the plan depend on the worker running long interactive
  commands or multi-step QEMU debugging in the same session as a code write.
