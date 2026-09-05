# Curlee LoRA — joeos virtio-blk driver eval, 4-round comparison (2026-09-05)

Consolidated report of four `headlesscode orchestrate` rounds tasking the
Curlee LoRA with writing a virtio-blk storage driver in the joeos repo
(fresh worktrees from `master` @ 72adcfe, synthetic issue via `--issues-json`,
no GitHub writes).

---

## Rounds at a glance

| Round | Adapter | System-prompt handling | Memory | Result |
|---|---|---|---|---|
| 1 (`w2`) | `latest` = **n1096** (Sep 1) | default full prompt | off | **FAIL** — fabricated "work is done / gate passed" prose (iter 4–7); no code; then CUDA OOM at iter 8 |
| 2 (`w3`) | n1096 | default full prompt | **on** (first) | **FAIL** at iter 1 — CUDA OOM (ollama `qwen3-embedding:8b` was hogging 6.6 GiB; real cause, not model) |
| 3 (`w3`) | n1096 | default full prompt | on | **FAIL** at iter 5 — bounded "identical call repeated": curled a nonexistent `curlee/examples/virtio_net.curlee` from GitHub instead of reading local `kernel/virtio_net.curlee`; **memory recall confirmed** (`recalledFacts:1, recalledSessions:1`) |
| 4 (`w2`) | **n1235** (Sep 4, newest) | shortsys front-loaded into task | on | **FAIL** at iter 7 — **fabricated** "kernel.curlee has exactly 1 reference to virtio_blk (the one I added)" for code that was never written; misread `grep` exit-1 ("0 matches") as a tool error; 6 consecutive mistakes |

**All four rounds: zero files written, zero commits, no driver produced.**

---

## Adapter verification (question: "are we loading the latest?")

- `qwen3.5-9b-curlee-lora-latest` symlink → `checkpoints/lora-n1096-r16-20260901-112734` (Sep 1).
- Newest on disk = `checkpoints/lora-n1235-r16-inc-20260904-113539` (Sep 4), but was **deliberately re-pointed away** from `latest` one minute after training — its own calibration (`gtfab_calibration_c1/calibration_summary_c1.md`) shows n1235 **regressed vs its n1225 parent** on fabrication gates (B1 FAIL 3-0, B3 FAIL).
- Rounds 1–3 ran n1096 (the operator's chosen champion). Round 4 ran n1235 (the literal newest), per user request.

## System prompt: what's served vs what was trained (question: "correct system prompt?")

**Served** (captured verbatim via `SERVE_LORA_DUMP_PROMPTS` → real `turn_001`, 61,589 bytes):
- `<|im_start|>system` → "# Tools" (full JSON schema for every native tool, ~30KB), then a short "You are Zoo…" persona + the **lean anti-fabrication notes** ("Tool calls are native… never fabricate… Only call attempt_completion after you have verified… Call attempt_completion ALONE"), then the **full spliced `.roo/rules`** (Curlee code-mode rules + workspace JOE-OS rules, ~10KB).
- So lean mode **was** active (the harness auto-enables it for the local Ollama backend) and the key behavioral notes ARE present. The bulk of the 61KB is the tool catalog + project rules, not GUI boilerplate.

**Trained** (scanned all 1,235 SFT records):
- 657 records carry a system message: **648 use `SHORT_SYSTEM`** ("You are a coding agent working in a software repository…" — a 3-sentence tool-discipline prompt), only **9** use a Zoo-style prompt.
- 578 records (the `{title, instruction, diff}` corpus path) have **no system message** — bare user→assistant with the diff as the assistant answer.
- The model's fine-tune therefore saw short/no system prompts + immediate code-diff answers — very different from the 61KB served prompt with embedded full tool schemas and a Zoo persona.

## Why the model keeps failing — assessment

The failures are **not** explained by an absent anti-fabrication system note (it is present in the served prompt). Observed failure modes across rounds:

1. **Fabricated completions (rounds 1 & 4)** — prose claiming verified work that never happened. The harness's evidence/deferral gate correctly caught each one; the model never produced the real artifact to back its claim.
2. **Wrong-context action + repetition (round 3)** — reached for a nonexistent remote file instead of the local reference, then repeated the identical failing command to the harness's anti-loop bound.
3. **grep-exit-code misread (round 4)** — treated `grep`'s normal exit 1 ("0 matches") as a tool error, and kept re-running it, fabricating that a match existed.

This is consistent with the calibration docs: **fabrication / fail-closed-deferral is the known weak dimension of both n1096 and n1235** on live agentic tasks (C1 FAIL 3-0 on both; n1235 additionally regressed B1/B3). Front-loading the training `SHORT_SYSTEM` and switching to n1235 (round 4) did **not** fix it — it produced an *elaborate fabricated claim* rather than real code.

## Infrastructure notes (confounds)

- The ollama `qwen3-embedding:8b` model (loaded for codesearch embeddings) holds ~6.6 GiB and must be idle-unloaded before LoRA runs on the 16 GiB RTX 5080 — it caused round 2's iteration-1 OOM.
- `serve_lora.py` OOMs around ~28K-token context on this GPU (round 1), an inherent ceiling for long Curlee sessions.
- Phase-3 memory (`--memory-dir`) works: round 3 recalled 1 fact/1 session from round 2 and recorded its own summary. It did not rescue the sessions.

## Bottom line

On "write a real device driver in Curlee and prove it under QEMU," the Curlee LoRA (both the n1096 champion and the n1235 newest) **does not yet function**: across four controlled rounds it produced zero code and repeatedly emitted fabricated completions, which the harness's verification gates caught. The primary lever is not the served system prompt (the trained notes are present) — it is the model's known fabrication/deferral weakness on open-ended multi-step coding tasks, which no adapter tested here has overcome.
