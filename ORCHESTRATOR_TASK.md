Project and mode-specific rules (from the central shared store and this project's own
.roo/rules*) are already spliced into your system prompt automatically — you do not
need to read any rules file yourself.

You are working autonomously in an isolated git worktree, on your own branch, with your own
isolated environment. No human is in the loop until you open a PR. Read this entire file before
doing anything.

## Your assignment

GitHub issues assigned to this worktree: #26. The real title/body of each is inline below.

### Issue #26: virtio-blk storage driver (raw sector read, no filesystem)

## Problem statement

joeos has no storage driver of any kind — every driver built so far is
framebuffer, serial, or network. This blocks loading anything from disk,
including model weights beyond what can be embedded directly in the kernel
image at compile time (which caps model size hard, especially against the
PVH LOAD-segment budget already documented as a hard constraint elsewhere in
this codebase).

## What to do

Follow the exact pattern already proven twice with `virtio_net.c`/
`virtio_net.curlee`: a paravirtualized VIRTIO device, well-documented,
already emulated by QEMU (`-device virtio-blk-pci`), so it can be developed
and tested the same way networking was — no real hardware needed until the
bare-metal-readiness milestone real-hardware validation work.

1. Check the language-gap status before starting: this should very likely
   be written DIRECTLY in Curlee, not C-then-ported, if the gaps that
   blocked virtio_net.c (address-of, runtime Phys read/write, module-level
   state) are all closed by the time this is picked up.
2. Implement PCI discovery + legacy or modern virtio-blk queue setup
   (mirrors virtio-net ring setup almost exactly — one request queue
   instead of separate RX/TX).
3. Minimal read-only surface for the MVP: read N sectors starting at a given
   LBA into a fixed buffer. No write support needed initially. No
   filesystem — that is a separate, later concern once there is a reason to
   need one (a raw "read this many bytes at this offset" contract is enough
   to load a single embedded model blob from a disk image, which is likely
   sufficient for the native-inference MVP).
4. A minimal build-side mechanism to put a model blob at a known LBA offset
   on the disk image (a small script, not a real filesystem).

## Acceptance criteria

- [ ] A virtio-blk device is discovered and initialized under QEMU
      (`-device virtio-blk-pci,drive=...`).
- [ ] Raw sector reads work: given an LBA and a sector count, the requested
      bytes land in a provided buffer, verified by a smoke test that reads
      a known pattern written into the disk image at build time.
- [ ] A new QEMU smoke gate proves this end-to-end, following the existing
      `qemu-net-smoke` pattern.

## Verification plan

QEMU boot with a disk image containing a known test pattern at a known
offset; the driver reads it and reports success via the existing serial
marker convention.

## Migration impact

Direct prerequisite for anything in the Native Inference milestone beyond a
toy compile-time-embedded model.

## Operational notes (read first, before any exploration)

- A PREVIOUS attempt at this same issue spent its ENTIRE session budget
  re-verifying curlee language basics (grepping the curlee compiler repo
  for "enum" support, re-reading files already read earlier in the same
  session) and NEVER WROTE A SINGLE LINE of kernel/virtio_blk.curlee. Do
  not repeat this. kernel/virtio_net.curlee already demonstrates every
  language feature this driver needs — read it ONCE, then start writing
  kernel/virtio_blk.curlee directly. Prefer writing code over further
  research once you have read virtio_net.curlee and the PCI/virtio setup
  it uses.
- kernel/virtio_blk.curlee does NOT currently exist in this repo. If you
  believe it already exists, you are wrong — verify with list_files before
  ever claiming it is already there.
- execute_command fails intermittently (roughly 30-50% of calls) due to a
  known, unrelated infrastructure issue (spawn ENOENT) — NOT a problem with
  your commands and NOT a sign the tool is broken. Simply retry the exact
  same command again; it very often succeeds on the next attempt. Do not
  stop to ask a human about this — just retry, same as you would retry a
  flaky network call.
- Commit your work incrementally as you make real progress (even a partial,
  compiling driver skeleton) rather than only at the very end — this way
  partial progress survives even if you run out of iterations or hit
  tooling trouble late in the session.


## One more operational note (added after round 17)

If attempt_completion gets deferred because your last execute_command
failed, do NOT just call attempt_completion again — it will keep being
deferred. Retry execute_command instead. If you retry the EXACT SAME
command twice in a row, the harness temporarily blocks execute_command
for a few turns as an anti-loop guard — if that happens, do something
useful with a DIFFERENT tool for one turn (read_file the file you are
about to verify, or list_files) rather than immediately re-trying
attempt_completion, then retry execute_command once it is available
again. A session was observed getting stuck alternating between
attempt_completion (deferred) and a blocked execute_command for 30+
iterations straight without escaping — do not repeat this; vary your
actions instead of repeating the same two calls back to back.


## Environment

Workspace root: this worktree (branch: current branch). All file/command tools are scoped to
this worktree. Commit per logical unit with the issue number in the message.

## Scratch

Any scratch/temporary files (probe scripts, one-off data dumps, intermediate output) go in
`<workspace>/.headlesscode/scratch/` — NEVER write to `/tmp` or any other path outside this
worktree. `/.headlesscode/` is gitignored, so scratch there needs no ignore rule.

## Workflow

1. Read each assigned issue's inline body above and the relevant source files.
2. Implement the change following the project's own conventions and the rules files.
3. Verify for real (run the project's tests / boot check) and capture the output.
4. Commit with the issue number referenced, one logical change per commit.
5. Push the branch and open a PR (ready, not draft).
6. Post the closing comment below on each issue, then close it.

## Required comment template

Your issue-closing comment must use this exact structure (every section present; a section can
be 'N/A' with a reason, but a missing section is itself a review finding):
```
## Issue <n> — Closing Report

### Files changed
<one line per file>

### Baseline (before)
<real output>

### Baseline (after)
<real output>

### Boot check
<real output or exact reason it didn't apply>

### Residual risk / notes
<anything not 100% certain>

PR: <link>
```

## Closing out

Once every assigned issue is closed with the template above:
1. Finish with attempt_completion and a full summary of what you changed, the real baseline
   numbers, the boot check, and the PR link(s).
2. The harness records your completion automatically (exit code + .harness.done marker) —
   no separate phone-home step is needed.
3. Do not start on any issue not assigned to this worktree.

Scope: #26 only.