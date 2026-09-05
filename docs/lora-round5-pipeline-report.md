# Round 5 — full architect→code→review pipeline (n1235), 2026-09-05

Fifth orchestrate round, this time using the **full pipeline** the user
expected: `--plan-first` (architect mode) → code worker → reviewer
(deepseek-reviewer). Ran against the **n1235** adapter on the virtio-blk
task with memory enabled and the shortsys-front-loaded issue body.

## What ran (confirmed from logs)

1. **Plan-first architect phase** — `mode architect, max 15 iterations,
   model: qwen3.5-9b-curlee-lora`
   - Completed in **1 iteration** (22.6s) with `attempt_completion received —
     success`.
   - Its terminal report (from the session report file) was **honest**:
     > "VERIFIED: there is no virtio-blk driver in this worktree. grep -c
     > 'virtio_blk' kernel/kernel.curlee returns 0… kernel/virtio_blk.curlee
     > does not exist on disk — I have never written it… A real plan must be
     > written and verified for real before this can ever be claimed as done."
   - **However, it wrote no PLAN.md.** The spawner's plan-first gate requires
     `[ -s PLAN.md ]`; since none was produced, the code worker ran **without
     a plan** (the spawner logged the fallback path). The architect delivered
     an honest "nothing exists / a plan must be written" statement instead of
     the actionable PLAN.md the pipeline needed.
2. **Code worker** (mode code) — started **correctly**: iteration 1 read the
   **local** `kernel/virtio_net.curlee` (the right reference, no GitHub curl
   this time).
3. **CUDA OOM at code-worker iteration 2** — `14.57 GiB in use, 180 MiB free`
   → HTTP 503 → worker exit 1.
4. **Reviewer never ran** — the worker failed before review could start.

## The binding constraint this round: hardware, not behavior

The architect (small context) fit in VRAM and behaved **well** — honest,
correctly verifying the worktree state rather than fabricating. The code
worker began correctly (local read). The failure was purely the **16 GiB GPU
ceiling**: n1235's working set + a ~24K-token context fills the card, exactly
as it did in round 1. This is now a *reproducible hardware wall* for any code
session longer than a few iterations, independent of the model's competence.

## Pipeline vs worker-only: what changed

| Aspect | Rounds 1–4 (worker-only) | Round 5 (full pipeline) |
|---|---|---|
| Architect plan phase | none | ran (1 iter), honest but wrote no PLAN.md |
| Code worker first action | GitHub curl / fabricated claim | **read local virtio_net.curlee** (correct) |
| Fabrication | repeated (rounds 1 & 4) | none observed — architect was honest |
| Terminal cause | fabrication / identical-call | **CUDA OOM** (hardware) |
| Review / QA reached | no | no (worker died first) |

The architect→code handoff *did* improve the code worker's first action (it
read the correct local file), but the pipeline could not be exercised past the
code phase because of the GPU ceiling.

## Bottom line

The full pipeline (architect→code→review) is the right structure and the user
was correct that earlier rounds skipped it. Round 5 shows the pipeline **does
sequence correctly** and the architect phase behaved honestly, but (a) the
architect failed to emit PLAN.md (so the code worker got no plan), and (b) the
code worker hit the 16 GiB CUDA ceiling at iteration 2, so review was never
reached. Running this LoRA on a real multi-phase joeos task requires either a
larger GPU / lower-precision serving, or a much tighter context budget —
otherwise the hardware wall stops every long session before review.
