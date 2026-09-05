# Reusable small-slice task template (for the local 9B LoRA)

This is the canonical task template that reliably gets the local Curlee LoRA
(n1235) to produce **correct, verified, committed** Curlee. Copy it, fill in
the bracketed parts, save it as a `--task-file`, and run a code-mode session
on a fresh worktree from master. See `docs/joeos-llm-agent-plain-english.md`
for the full context (why this works, the model's limits, the harness).

## The template

```markdown
You are a coding agent working in a software repository. Tools are available
to you (read_file, write_to_file, edit_file, execute_command, list_files,
attempt_completion, ask_followup_question, and others). Tool calls are native:
call a tool directly through the tool-calling mechanism. Writing out a tool
call, or a final answer, as plain JSON text instead of a real tool_calls entry
is a mistake -- it does not run. Only call attempt_completion once you have
actually verified your work; it must be called alone, via a real tool call,
never as prose.

IMPORTANT tool note: In this environment edit_file is UNRELIABLE and often
fails. Use write_to_file INSTEAD for new files -- it always works. Never use
edit_file.

STYLE RULE: Use `//` line comments ONLY. Never `///` and never `/* */` --
Curlee rejects both.

## Task (ONE small slice -- [one sentence: what to create/change])

[3-8 sentences: EXACTLY what to write. Give the precise function signatures,
contracts, and semantics. Reference the existing file that sets the house
style (e.g. "match kernel/virtio_blk_helpers.curlee exactly").]

Then:
1. Run `/home/joe/Projects/curlee/build/linux-debug/curlee check
   kernel/<file>.curlee` -- MUST exit 0. Fix and re-run if it errors.
2. Commit: `git add kernel/<file>.curlee && git commit -m "<msg>"`.
3. Verify: `git log --oneline -1`.

The task is NOT done until: [the file/change exists with the exact functions
above] AND curlee check exits 0 AND git log shows your new commit. Do not
touch any other file. Do not call attempt_completion before all three are
true.
```

## How to launch a slice session

```bash
cd /home/joe/Projects/joeos
# 1. free the GPU (the ollama embed reloads and eats 6 GiB)
ollama stop qwen3-embedding:8b
# 2. fresh worktree from master
git worktree add -b eval-sliceN .worktrees/eval-sliceN
cp plans/parallel-tasks/sliceN-task.md .worktrees/eval-sliceN/TASK.md
# 3. run the session (serve_lora must be up on :11436 with n1235 + 24K cap)
HEADLESSCODE_CODE_MODE_BACKEND=ollama \
HEADLESSCODE_LOCAL_BACKEND_MODES=code \
HEADLESSCODE_OLLAMA_URL=http://localhost:11436 \
HEADLESSCODE_CODE_MODE_MODEL=qwen3.5-9b-curlee-lora \
ALLOW_UNINDEXED=1 HEADLESSCODE_MAX_ITERATIONS=40 \
setsid nohup headlesscode --task-file /home/joe/Projects/joeos/.worktrees/eval-sliceN/TASK.md \
  --workspace /home/joe/Projects/joeos/.worktrees/eval-sliceN --mode code \
  --model qwen3.5-9b-curlee-lora --memory-dir /home/joe/.local/share/headlesscode/memory \
  > /tmp/sliceN.log 2>&1 < /dev/null &
# 4. when done: verify content MANUALLY (function list vs task spec), merge:
git merge --ff-only <commit> && git worktree remove --force .worktrees/eval-sliceN
```

## Rules that make it work

1. **One tiny slice per session** — one new file or one tiny change. Never a
   whole driver, never multiple files.
2. **Use `write_to_file` for new files**; the model's `edit_file` calls fail
   on optional-param schema (harness-documented).
3. **Require a git commit with the new content** as part of definition-of-done
   — this is the artifact gate that catches "verified but didn't do."
4. **Give exact signatures + contracts** — the model copies them reliably.
5. **`curlee check` takes ONE file** — one file per check invocation.
6. **Manually eyeball the result** — the model can emit wrong-but-verifiable
   content (e.g. a duplicate of an earlier similar file). Compare the
   function list to the task spec before merging.
7. Clean `///` doc-comments if the model adds them (Curlee wants `//`).

## What the model CANNOT do (avoid these task shapes)

- Edit a large existing file (Makefile, kernel.curlee) — it stalls.
- Multi-step tasks spanning several files.
- Long-horizon work needing >~40 iterations.
- Anything requiring runtime `make check`-style full builds it can't see pass.
