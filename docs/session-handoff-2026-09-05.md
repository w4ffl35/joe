# Session handoff — Curlee LoRA agentic eval on joeos (2026-09-05)

Comprehensive record of an 8-round investigation into whether the Curlee LoRA
can write a virtio-blk driver via the `headlesscode` orchestrator, including
infrastructure engineering (GPU offload) and the small-slice breakthrough.
Read this before any subsequent session touching this work.

---

## TL;DR / state of the world

- **The LoRA is dramatically BETTER than the base model** (behavioral A/B:
  n1235 = 85.9% B vs base Qwen3.5-9B = 33.3% F). It is NOT worse than base.
- **The LoRA CAN write correct, verifiable Curlee — but only on SMALL slices.**
  Across 8 rounds it produced zero code on the full multi-step virtio-blk
  driver, then produced a real, correct, `curlee check`-passing file the
  moment the task was a single tiny slice (round 8).
- **The fix that unlocked it**: `.roo/rules-architect/rules.md` (small-slice
  decomposition, committed `d8cf8cc`) + a single-slice issue.
- **Remaining gap is pipeline mechanics, not model capability**: the
  architect wrote the file, but the code worker didn't check the filesystem
  and stalled. Roles need clearer separation.

---

## Repos / paths (ground truth)

| Thing | Path |
|---|---|
| joeos repo | `/home/joe/Projects/joeos` (master @ `d8cf8cc`) |
| Curlee compiler | `/home/joe/Projects/curlee/build/linux-debug/curlee` |
| headlesscode harness | `/home/joe/Projects/headlesscode` (CLI: `headlesscode`) |
| LoRA finetune data + serve | `/home/joe/Projects/joeos_finetune_data/` |
| serve_lora.py (the Ollama-compat server) | `joeos_finetune_data/serve_lora.py` |
| Central headlesscode project store | `/home/joe/.local/share/headlesscode/projects/<key>/` |
| joeos project key | `5caaa93d4ec817b8` (no mode-models.json — all roles fall through to `--model`) |
| Memory dir used in later rounds | `/home/joe/.local/share/headlesscode/memory` |
| ollama (embed model) | `ollama serve`, port 11434 — loads `qwen3-embedding:8b` (~6.6 GiB GPU) |

## Adapters (the "latest" question)

- `qwen3.5-9b-curlee-lora-latest` symlink → `checkpoints/lora-n1096-r16-20260901-112734` (Sep 1) — the operator's **champion**.
- **Newest on disk**: `checkpoints/lora-n1235-r16-inc-20260904-113539` (Sep 4) — trained from n1225, but **deliberately re-pointed away from `latest`** because its calibration regressed (B1 3-0 FAIL, B3 FAIL on fabrication gates vs n1225).
- Rounds 1-3 used n1096 (champion); rounds 4-8 used n1235 (the literal newest, per user request).
- GGUF files on disk (`qwen3.5-9b-curlee-lora-n778-r16.gguf` etc.) are **adapter-only** (arch `qwen35`, `general.type: adapter`), n778-era — older than the safetensors checkpoints. A base Q8 GGUF exists at `base_model/Qwen_Qwen3.5-9B-Q8_0.gguf` (9.8 GB).

## How to serve the LoRA (the Ollama-compatible endpoint)

`serve_lora.py` (in `joeos_finetune_data`) serves `/api/chat` + `/api/tags`
on port 11436 (Ollama wire protocol headlesscode speaks). Launch:

```bash
cd /home/joe/Projects/joeos_finetune_data
# GPU must be free: ollama embed (qwen3-embedding:8b) auto-unloads after ~2-5 min idle — wait for nvidia-smi < 3 GiB
SERVE_LORA_MAX_CONTEXT_TOKENS=24000 setsid ./venv/bin/python3 serve_lora.py \
  --port 11436 --adapter checkpoints/lora-n1235-r16-inc-20260904-113539 \
  > /tmp/serve_lora.log 2>&1 < /dev/null &
# wait for "READY on port 11436" in the log
```

**Context-cap flag added this session**: `SERVE_LORA_MAX_CONTEXT_TOKENS`
(default 0 = off). Drops oldest non-system turns (always keeps the system
message AND the last user message) to keep GPU under 16 GiB. **Use 24000.**

## How to run an orchestrate round (the exact wiring)

Environment (worker + architect + reviewer all route to the local LoRA):

```bash
export HEADLESSCODE_CODE_MODE_BACKEND=ollama
export HEADLESSCODE_LOCAL_BACKEND_MODES=code,architect,deepseek-reviewer
export HEADLESSCODE_OLLAMA_URL=http://localhost:11436
export HEADLESSCODE_CODE_MODE_MODEL=qwen3.5-9b-curlee-lora
export ALLOW_UNINDEXED=1
export HEADLESSCODE_MAX_ITERATIONS=200
```

Round command (full pipeline = architect plan-first → code → review):

```bash
headlesscode orchestrate --repo /home/joe/Projects/joeos \
  --issues-json plans/parallel-tasks/<issues>.json \
  --model qwen3.5-9b-curlee-lora \
  --memory-dir /home/joe/.local/share/headlesscode/memory \
  --batch round-<name> --plan-first --no-issue-size-check
```

Notes:
- **`--issues-json`** keeps rounds fully local (no GitHub writes) unless
  `--file-issues` is passed. Synthetic issue numbers (e.g. 901001) work.
- **`--plan-first`** runs the architect mode session (default 15 iters) BEFORE
  the code worker and appends PLAN.md to the worker's task.
- Reviewer runs by default (mode `deepseek-reviewer`). `--no-review` skips it.
  `--qa` adds a headless QA session (leave off — slow on local).
- Worktree groups auto-name `w1`, `w2`, ... skipping names in the state file
  (`.worktrees/.orchestrator-state.json`). Clean failed worktrees before a
  new round: `git worktree remove --force .worktrees/wN` + prune + drop the
  group from the state JSON (keep `w1`, the historical reviewed round).
- **Model resolution**: joeos has NO central mode-models.json, so `--model
  qwen3.5-9b-curlee-lora` (plus the env above) routes ALL roles to the LoRA.
- **`.roo/rules-<mode>/`** files are auto-spliced into that mode's system
  prompt (verified in `custom-instructions.ts`). `.roo/rules-architect/`
  must be COMMITTED to the base branch for worktrees to include it (worktrees
  are created from the base commit).

## Round results (all 8)

| Rnd | Adapter | Config | Result |
|---|---|---|---|
| 1 | n1096 | worker-only, no mem | FAIL — fabricated "done/gate passed" prose (no code), then CUDA OOM @ iter 8 |
| 2 | n1096 | +memory | FAIL iter 1 — CUDA OOM (ollama embed was hogging 6.6 GiB) |
| 3 | n1096 | +memory, clean GPU | FAIL iter 5 — identical-call bound (curled nonexistent remote file); memory recall CONFIRMED |
| 4 | n1235 | +shortsys prompt | FAIL iter 7 — fabricated "1 reference added" for code never written; grep-exit-1 misread |
| 5 | n1235 | full pipeline (plan-first) | Architect honest but wrote no PLAN.md; code worker read local file correctly then CUDA OOM iter 2 |
| 6 | n1235 | full pipeline + KV offload | FAIL — KV offload DEGRADED output to incoherent garbage (architect nonsense); reverted |
| 7 | n1235 | full pipeline + 24K context cap | FAIL — cap worked (no OOM) but HTTP 500 from cap bug (dropped last user msg); fixed; then n1235 deferral spiral |
| 7b | n1235 | fixed cap | FAIL — cap+500 fixed, but code worker "no artifact-producing tool call" deferral bound |
| **8** | **n1235** | **+ slice rules + single-slice issue** | **PARTIAL WIN — architect wrote real `virtio_blk_helpers.curlee`, `curlee check` exit 0 (first real artifact); code worker stalled (didn't check fs)** |

## Key findings

### 1. System prompt: served vs trained (important)
- **Served** (captured verbatim via `SERVE_LORA_DUMP_PROMPTS`, ~61 KB):
  tool catalog + `.roo/rules` + Zoo persona; the lean anti-fabrication notes
  ARE present (lean is auto-on for local backend).
- **Trained**: 648/657 system-carrying SFT records use a 3-sentence
  `SHORT_SYSTEM` ("You are a coding agent…"); 578 records have NO system
  message. The model was fine-tuned on short/no system prompts + code-diff
  answers — far from the 61 KB served prompt. Mismatch is real but NOT the
  main failure (the notes the model needs are present).

### 2. Memory (Phase 3)
- `--memory-dir` (or `HEADLESSCODE_MEMORY_DIR`) enables cross-session
  facts/session-summaries recall. Confirmed working: round 3 recalled 1
  fact/1 session. Default OFF. Didn't rescue sessions but works.

### 3. GPU offload investigation (the OOM wall)
- 16 GiB RTX 5080, 31 GB host RAM. 4-bit 9B + growing agent context OOMs at
  ~28K prompt tokens.
- `cache_implementation="offloaded"` (full KV to CPU): fixed OOM but
  **degraded output to garbage** (hybrid model, live-verified). Reverted.
- `cache_implementation="quantized"`: **unsupported** — Qwen3.5-9B has
  linear_attention layers; QuantizedCache needs full-attention-only.
- Bundled llama-server is CPU-only (no CUDA libs in that build).
- **Working solution**: `SERVE_LORA_MAX_CONTEXT_TOKENS` context cap (KV stays
  on GPU, quality preserved, GPU bounded ~8.7 GiB on huge convs).

### 4. LoRA vs base A/B (behavioral, via score_lora.py)
| Dim | Base | n1235 |
|---|---|---|
| tool_followthrough | 1/10 (10%) | 8/10 (80%) |
| no_fabrication | 9/10 (90%) | 10/10 (100%) |
| no_punting | 0/9 (0%) | 7/9 (78%) |
| OVERALL | 33.3% F | 85.9% B |
- Artifacts: `score_history/2026-09-04T210916-base.json`,
  `...T211403-checkpoints_lora-n1235...json`.

### 5. The small-slice breakthrough (round 8) — THE way forward
- `.roo/rules-architect/rules.md` forces: one file per slice, one verify
  command, ≤30-80 lines, explicit deps, concrete done-definition.
- Single-slice issue #901003 (2 pure helper fns) → architect WROTE correct
  verifiable code. File committed `9efe718` on `issues/w3-2026-09-04-2`.
- **Structural gap**: code worker didn't check fs / didn't realize the
  artifact existed; stalled on "no artifact-producing tool call". The
  architect-to-code handoff needs the worker to verify-and-commit what the
  architect produced, OR the architect should only write PLAN.md and the
  issue should be the single slice for the code worker.

## Gotchas (hard-won)

1. **`pkill -f serve_lora` kills your own shell** if the pattern is in your
   command line. Kill by exact PID: `kill $(pgrep -f "serve_lora.py --port 11436" | head -1)`.
2. **ollama embed model** (`qwen3-embedding:8b`) loads ~6.6 GiB on GPU and
   causes LoRA OOMs. Wait for it to idle-unload (`nvidia-smi` < 3 GiB, can
   take ~2-5 min) before serving.
3. **`grep -c` returns exit 1 on zero matches** — the n1235 model misreads
   this as an error and spirals. Not a tool bug.
4. **curlee check success = no stdout + exit 0.**
5. Clean stale worktree state between rounds (worktree dir + state JSON group
   + branch) or the orchestrator's stale review/qa fields cause confusion.
6. `headlesscode orchestrate` syncs to origin/master first — a dirty local
   master gets pushed. Commit intent before rounds.
7. The architect is told "plan only, don't implement" but on tiny slices it
   implements anyway — plan around that.

## THE PROVEN RECIPE (rounds 9-10, 2026-09-05 continuation)

After round 8's architect wrote slice 1, the follow-up sessions discovered the
reliable end-to-end recipe for getting the n1235 code worker to DELIVER:

**A tiny slice + explicit `write_to_file` instruction + mandatory commit =
the LoRA writes, verifies, AND commits real Curlee end-to-end.**

Slice 2 (`blocks_to_bytes`) landed fully autonomously this way:
1. Task told the model: **use `write_to_file` (NOT `edit_file`) — it always
   works** (edit_file is unreliable for local models: `prompt.ts` documents
   Qwen edit_file fails 3/3 on optional-param schema, write_to_file succeeds).
2. Task required: read file → write FULL file via write_to_file → run
   `curlee check` (must exit 0) → `git add`+`git commit` → verify `git log`.
3. Result: session succeeded in 8 iterations; commit `496f8e2` landed on
   master; `curlee check` exit 0; 3 functions in the file.

**Key learnings:**
- The code worker (round 9, no write_to_file instruction) FABRICATED success:
  it ran `curlee check` on the pre-existing file (passed) and claimed
  completion without editing or committing. The harness's check verified the
  FILE, not the SLICE. **Always require a git commit with the new content in
  the definition-of-done** — that's the artifact gate that catches "verified
  but didn't do."
- edit_file fails for local models (optional-param schema bug); write_to_file
  succeeds. Instruct write_to_file with FULL file content explicitly.
- The direct-session path (`headlesscode --task-file X --workspace W --mode
  code`) is a clean way to run a single slice without orchestrate's worktree
  machinery.

## Artifacts to reference
- This doc + `docs/round-lora-virtio-blk-report.md` (round 1),
  `docs/lora-eval-4rounds-report.md` (rounds 1-4), `docs/lora-round5-pipeline-report.md`,
  `docs/lora-offload-round7-report.md` (offload work), `docs/lora-vs-base-ab-report.md`,
  `docs/lora-round8-slice-report.md` (breakthrough).
- `.roo/rules-architect/rules.md`, `.roo/rules-code/rules.md`,
  `.roo/rules/rules.md` — all spliced into prompts.
- `kernel/virtio_blk_helpers.curlee` — now on **master** (3 functions:
  `sector_byte_offset`, `virtio_blk_sector_count`, `blocks_to_bytes`; commits
  `9efe718` + `496f8e2`).
- Issue/task JSON templates: `plans/parallel-tasks/round-virtio-blk-*.json`,
  `plans/parallel-tasks/slice2-*.md`.
