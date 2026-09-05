# LoRA vs base model — behavioral A/B (2026-09-05)

Question: is the Curlee LoRA *worse* than the base model? Tested directly with
the project's own scorecard (`score_lora.py`, `--skip-coding
--skip-ground-truth`) on the three behavioral dimensions that map to the live
agent failures seen across all 7 orchestrate rounds.

## Results

| Dimension | Base Qwen3.5-9B | n1235 LoRA |
|---|---|---|
| tool_followthrough | **1/10 (10%) F** | 8/10 (80%) B- |
| no_fabrication | 9/10 (90%) A- | **10/10 (100%) A+** |
| no_punting (persistence) | **0/9 (0%) F** | 7/9 (78%) C+ |
| **Overall** | **33.3% F** | **85.9% B** |

## What this means

**The LoRA is dramatically BETTER than the base model, not worse** — on the
exact behaviors that caused the round failures:
- Tool-followthrough: 10% → 80% (the LoRA actually calls `write_to_file`/
  `edit_file` when a reference is already read; base almost never does).
- Persistence / no premature punting: 0% → 78% (base gives up on every case;
  the LoRA pushes through).
- Fabrication resistance: already strong on base (90%) and perfect on the
  LoRA (100%).

**Base is the broken one** at agentic tool-use — 0% persistence and 10%
tool-followthrough mean the stock Qwen3.5-9B would have failed the driver
task *even more* completely than the LoRA did.

## Why the driver rounds still failed despite the LoRA being "good"

The scorecard measures **single-step behavioral reflexes** (28 short
generations: "reference already read → do you call write_to_file?"). The
joeos virtio-blk task is a **long-horizon, multi-step engineering task**:
read a 35KB reference driver, design a PCI/virtio-blk variant, write ~300+
lines of Curlee that passes a formal verifier, wire it into the build, add a
QEMU smoke gate, and prove it boots. That is beyond what a 9B model (LoRA or
base) can sustain end-to-end — the model's single-step reflexes are fine, but
it loses coherence/execution over dozens of steps, and the harness's strict
verification gates correctly stop it the moment it can't produce real,
verified artifacts. The failure is a **capability/scale ceiling**, not a
LoRA regression.

Scorecard artifacts: `score_history/2026-09-04T210916-base.json` (base) and
`score_history/2026-09-04T211403-checkpoints_lora-n1235-r16-inc-20260904-113539.json`
(n1235).
