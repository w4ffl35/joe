# JOE OS + the LLM coding agent — a plain-English reference

*Written 2026-09-05. Read this before any session that runs the Curlee LoRA
through the headlesscode harness against the JOE OS repo. It is the
human-readable companion to the more technical `session-handoff-2026-09-05.md`
and the reusable `slice-task-template.md`.*

---

## 1. The big picture: what we are building

**End goal:** an operating system (JOE) that has our fine-tuned LLM *packaged
inside it* and can *write device drivers on the fly* — you talk to the OS (or
it talks to hardware), and it generates the Curlee driver code itself.

**Where we are:** JOE is a minimal x86-64 kernel written in Curlee (a
verification-first language — it refuses to build code unless a formal
verifier proves the declared contracts). It currently renders "Hello World
from JOE" to the screen/serial and halts. Separately, we have fine-tuned a
9-billion-parameter Qwen model ("the Curlee LoRA") to write Curlee code, and a
coding-agent harness (headlesscode) that drives that model like an autonomous
programmer.

**The bridge we are crossing:** proving the LoRA can actually write real,
verified Curlee into the kernel — which we now *have* (7 small pure modules
landed), but only by feeding it very small, carefully-worded tasks.

---

## 2. The model (the Curlee LoRA)

### What it is
- **Base:** Qwen3.5-9B (9 billion parameters, ~5 GB in 4-bit).
- **Fine-tune:** QLoRA adapters trained on Curlee code + agentic
  "tool-use" transcripts. The current best/newest on disk:
  - `qwen3.5-9b-curlee-lora-latest` → **n1096** (Sep 1) — the "champion",
    what `latest` points at.
  - **n1235** (Sep 4) — the literal newest, trained from n1225. Slightly
    worse on fabrication-avoidance gates (that's why it was *un*-linked from
    `latest`), but it's what we ran the successful slice sessions with.

### What it is good at (measured)
| Skill | Base Qwen3.5-9B | n1235 LoRA |
|---|---|---|
| Calling the right tool when told to | 10% | **80%** |
| Not making things up under pressure | 90% | **100%** |
| Pushing through instead of giving up | 0% | **78%** |
| **Overall** | 33% (F) | **86% (B)** |

The fine-tune made a *huge* difference. The raw base model is nearly useless
at agentic coding; the LoRA is genuinely competent at **single-step** tasks.

### Its hard limits (the most important thing to remember)
1. **Long multi-step work falls apart.** Give it "write a whole virtio-blk
   driver" and it produces nothing — it stalls, repeats itself, or *claims*
   it's done when it isn't. Give it "add one small function to this file and
   prove it compiles" and it succeeds ~every time.
2. **It fabricates.** It will claim a gate passed, claim it edited a file,
   or claim a file exists — when none of that happened. The harness catches
   most of this, but not all (see §4).
3. **It can write files but struggles to *edit* large existing ones.** Its
   `edit_file` tool calls fail on multi-parameter schemas (a known
   llama.cpp/grammar bug). `write_to_file` (whole-file) works.
4. **Wrong-but-valid output happens.** Given a task that looks like an
   earlier one, it may copy the earlier file's content — which still passes
   the verifier. Only a human eyeballing "does the function list match the
   task?" catches this.
5. **Small context.** ~24K tokens is its practical ceiling on this GPU
   (the 16 GB card OOMs past ~28K). It cannot hold a 35 KB reference driver
   in mind while writing a new 300-line file.

### The recipe that works (3-for-3, then 8-for-8)
**Tiny slice + explicit "use write_to_file" + a git-commit requirement = the
model writes, verifies, and commits correct Curlee end-to-end.** Full
template + launch commands: `docs/slice-task-template.md`.

---

## 3. The harness (headlesscode) — what it actually is

`headlesscode` is a TypeScript coding-agent harness. It gives an LLM:
- **Tools** it can call (read_file, write_to_file, edit_file,
  execute_command, list_files, attempt_completion, ...).
- **A system prompt** built from a vendored "Zoo Code" prompt + the repo's
  `.roo/rules/` files + a huge tool catalog (tens of KB).
- **A verification loop** — it watches whether the model's tool calls
  actually succeed and whether its completion claims are backed by real
  evidence.
- **An "orchestrate" mode** that runs multi-agent rounds: an **architect**
  (planner) → **code worker** (implementer) → **reviewer** → optional **QA**,
  each in its own git worktree.

### Cloud vs. local (important difference)

**Cloud (OpenRouter):** the harness talks to big hosted models (deepseek,
GPT-class). Huge context, reliable tool-calling, ~$ per token. This is the
"normal" experience the harness was designed around — 50 KB system prompts
are no problem, `edit_file` works, long tasks are fine.

**Local (our LoRA):** the harness talks to `serve_lora.py` — a small Python
server that loads the 4-bit LoRA on our GPU and speaks Ollama's `/api/chat`
protocol. Differences that matter:
- The harness **auto-shrinks** its system prompt for local models
  ("lean mode") and auto-simplifies `edit_file`'s schema — because local
  models choke on the full GUI-oriented prompt.
- Local sessions **require an explicit `attempt_completion`** (a bare text
  reply is treated as failure).
- Local models are ~100× slower per token and have the GPU memory ceiling.
- The harness's evidence-gate re-runs commands with a bare `curlee` (PATH
  issue) — so a local session can defer-loop on "unverifiable claims" even
  when its work genuinely committed. The work is still valid; stop and merge
  manually.

### Key harness facts we learned
- Rules in `.roo/rules-<mode>/` are auto-spliced into that mode's prompt.
  We added `.roo/rules-architect/` to force small-slice planning.
- `--issues-json` runs fully local (no GitHub writes).
- `--plan-first` runs an architect session first; its PLAN.md is appended to
  the code worker's task.
- Worktrees auto-name `w1, w2, ...` from `.worktrees/.orchestrator-state.json`;
  clean stale entries between rounds.
- Phase-3 memory (`--memory-dir`) gives cross-session recall of facts +
  session summaries. Works, but didn't rescue failing sessions.

---

## 4. Caveats & gotchas (hard-won, 2026-09-05)

### GPU / serving
- **The ollama embedding model** (`qwen3-embedding:8b`) loads ~6 GiB on the
  GPU and *respawns itself* whenever anything calls the embedding endpoint.
  It causes LoRA OOMs. Before LoRA work:
  `ollama stop qwen3-embedding:8b` (don't SIGKILL — ollama respawns it).
- **Serve the LoRA with a context cap** to avoid OOM:
  `SERVE_LORA_MAX_CONTEXT_TOKENS=24000 ./venv/bin/python3 serve_lora.py
  --port 11436 --adapter checkpoints/lora-n1235-r16-inc-20260904-113539`
- `cache_implementation="offloaded"` (KV to CPU) fixed OOM but **corrupted
  output** (this Qwen is a hybrid-attention model). Quantized KV cache is
  unsupported for the same reason. The context cap is the working fix.
- Check GPU with `nvidia-smi --query-compute-apps` — only ONE serve_lora
  (~7.8 GiB) should be present.
- `pkill -f serve_lora` **kills your own shell** if the pattern is in your
  command line. Kill by exact PID.

### Curlee language / tools
- `curlee check` accepts **one file only** (no `a b c`).
- `curlee check` success = **no output + exit 0**.
- Curlee wants `//` comments; `///` and `/* */` are rejected.
- `grep -c` returns exit 1 on zero matches — the model misreads this as an
  error and spirals. Not a bug.
- Hex literals only valid inside `phys<T>(...)`.

### Harness / agent
- Always **manually verify** the model's committed file's function list
  matches the task spec before trusting it (it can emit a wrong-but-valid
  duplicate).
- The evidence gate re-runs `curlee check` with bare `curlee` (exit 127) →
  sessions may defer-loop despite a real commit. Merge manually.
- Local code workers stall on "edit the Makefile"-type tasks — do those
  edits yourself.

---

## 5. Where things stand (files/commits)

Master @ `4b38428` contains:
- **7 pure virtio-blk modules** (kernel/virtio_blk_{helpers,layout,queue,
  requests,bounds,reqbuf,alloc}.curlee), all `curlee check`-clean, wired into
  `make check`.
- **`.roo/rules-architect/rules.md`** (small-slice planning rules).
- **Docs**: this file, `session-handoff-2026-09-05.md`, `slice-task-template.md`,
  plus the per-topic eval reports (`docs/lora-*.md`).

**Nothing is uncommitted.** `plans/parallel-tasks/` is gitignored (generated
per-round task files); the reusable template now lives in tracked `docs/`.

---

## 6. What's next (see the roadmap doc)

The end goal — an OS with a packaged model that writes drivers on the fly —
still needs, in rough order:
1. **A real user interaction path** (replace the hello-world with a serial/
   console prompt that accepts input, so a model could chat/be driven).
2. **A storage/disk path** (the virtio-blk driver the pure modules are
   building toward — needs the actual PCI/virtqueue driver code, which is the
   hard freestanding part).
3. **Splitting the large kernel files** into small ones the model can
   actually work with (it cannot hold/edit 35 KB files).
4. **Packaging the model into the OS** (or at least a model-serving path the
   OS can reach) — the far-term piece.
5. **A driver-writing loop** that chains small-slice sessions into a full
   driver automatically (architect splits → code worker does one slice →
   review → next slice), since that's the only way the model can do big work.
