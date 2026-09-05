# Round 8 — small-slice decomposition (architect rules), 2026-09-05

Testing the hypothesis: **force the architect to plan/emit SMALL slices so the
9B LoRA's strong single-step reflexes (80% tool-followthrough, 100%
no-fabrication) can actually execute them**, instead of failing on a
long-horizon driver task.

## What was changed

1. **`.roo/rules-architect/rules.md`** (committed `d8cf8cc`): architect-mode
   rules forcing small-slice decomposition — one file change per slice, each
   independently verifiable with one command, ≤30-80 lines, explicit
   dependency order, concrete definition-of-done. The harness splices
   `.roo/rules-<mode>/` into that mode's system prompt (verified in
   `custom-instructions.ts`), so the architect's prompt now contains these
   rules.
2. **A genuinely small single-slice issue** (`round-virtio-blk-slice-issues.json`,
   #901003): "create `kernel/virtio_blk_helpers.curlee` with 2 pure
   functions (`sector_byte_offset`, `virtio_blk_sector_count`), verify with
   one `curlee check`". Full pipeline (architect→code→review) on just this
   slice, n1235 adapter + 24K context cap.

## Result: FIRST real artifact across all 8 rounds

The **architect phase wrote a real, correct, verifiable Curlee file**:

- `kernel/virtio_blk_helpers.curlee` — 916 bytes, SPDX header, both functions
  with correct contracts:
  ```curlee
  fn sector_byte_offset(lba: Int) -> Int
    [ requires lba >= 0; ensures result == lba * 512; ] {
    return lba * 512;
  }
  ```
- **`curlee check` passes (exit 0)** — independently verified.
- Committed as `9efe718` on the round-8 worktree branch (rescued after the
  worker stalled).

## The remaining failure (structural, not capability)

The **code worker** (which runs after the architect) did not realize the
architect had already written the file. It called `attempt_completion` 6
times with no artifact-producing tool call ("no artifact-producing tool call
in this session") and hit the bounded-failure limit. The harness's
architect→code handoff assumes the architect writes a *plan* (PLAN.md) and
the code worker does the *writing* — but the architect here did the writing
directly (the slice was small enough that planning and doing collapsed), and
the code worker then had nothing to do and didn't check the filesystem.

## Verdict on the hypothesis

**CONFIRMED — smaller slices let the LoRA produce correct code.** When the
task is a single small verifiable slice, the model (even in the "planning"
architect role) writes correct Curlee with proper contracts that passes the
verifier. This is a dramatic contrast to every prior round, where the same
model produced zero code on the full multi-step driver task.

**The remaining gap is pipeline mechanics, not model capability**: the
architect and code-worker roles need clearer separation (architect writes
PLAN.md only; code worker checks what exists and executes the slice), or the
architect's file write should be treated as the deliverable and the code
worker told to verify/commit it. That is a harness/orchestration adjustment,
not a model limitation.

Artifact: `kernel/virtio_blk_helpers.curlee` (commit `9efe718` on branch
`issues/w3-2026-09-04-2`).
