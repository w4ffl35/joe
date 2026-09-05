# Task: simplify the large kernel files so the on-OS model can work with them

**Status:** proposed GitHub issue / workstream C of `docs/roadmap-to-end-goal.md`.
**Why:** our fine-tuned 9B coding model (the Curlee LoRA) can reliably write
NEW small files, but cannot hold or edit large ones. Several kernel files are
800-1000+ lines. To let the model (packaged in the OS, or driving the OS over
serial/net) write drivers and modify the kernel on the fly, the code it must
touch has to live in small, single-responsibility modules.

## Ground rules (read first)

1. **Only rewrite files we intend to keep.** We are NOT preserving the
   "Hello World from JOE" one-shot boot as the end state — we are moving
   toward an interactive LLM console. So `kernel/kernel.curlee`'s giant
   `main` + marker functions should be reorganized, not preserved as-is.
2. **Do not change behavior or contracts.** Splitting is a pure
   refactor: the merged kernel must still build and every existing gate
   (`make check`, `make qemu-smoke`, `qemu-net-smoke`, etc.) must stay green.
3. **Every new file must be individually `curlee check`-clean** (curlee check
   takes ONE file) and wired into `scripts/build-kernel.sh`'s module list +
   the Makefile `check:` target, in dependency order.
4. **Keep the merge order in `scripts/build-kernel.sh`** (dependencies first)
   correct — a split module must be listed before anything that calls it.
5. **House style:** `//` comments only, SPDX header, 2-space indent, explicit
   contracts, explicit `return;`.

## Which files to split (current sizes)

| File | Lines | Why split |
|---|---|---|
| `kernel/net_stack.curlee` | 984 | TCP/ARP/HTTP stack — split into protocol-layer modules (arp, tcp, http, ip) |
| `kernel/virtio_net.curlee` | 892 | net driver — split probe/setup vs ring vs rx/tx vs markers |
| `kernel/kernel.curlee` | 833 | main + ~20 serial_*_marker functions + bringup helpers — extract markers + per-device bringup into their own files; main becomes a thin orchestrator that also (later) enters the chat loop |
| `kernel/json.curlee` | 692 | json parser — split lexer/parser/value |
| `kernel/fb.curlee` | 688 | framebuffer — split canvas/glyph/blit layers |
| `kernel/net_glue.curlee` | 522 | glue — mostly extern shims + state; may shrink naturally |

(Test-only files `canvas_test/json_test/net_stack_test` are VM-run and can
stay as-is, but note they will need updating if the modules they test move
functions.)

## What "simplified" means (target shape)

- Each file is **one clear responsibility**, ideally **under ~150 lines**.
- **No file over ~250 lines** in the final state (the model's comfortable
  ceiling is well under the current 800-1000).
- Pure logic is separated from Phys/port-I/O/unsafe code, so the pure parts
  stay VM-verifiable and slice-friendly.
- The giant `main` becomes: call each device's `bringup()`, then (future)
  enter the interactive loop — not 200 lines of inline marker code.

## Suggested slice plan (do it as slices, not one giant rewrite)

Because the rewrite itself must be verifiable at each step, do it in the
same small-slice style that built the virtio-blk modules:

1. Extract the ~20 `serial_*_marker` functions out of `kernel.curlee` into
   `kernel/serial_markers.curlee` (mechanical move; `make check` + smoke
   gates must stay green).
2. Extract each device's bringup from `main` into `kernel/<dev>_bringup.curlee`
   (e.g. `e1000_bringup`, `net_bringup`, `fb_bringup`) — one slice per device.
3. Split `net_stack.curlee` by protocol layer — one slice per layer, keeping
   the merged TU green after each.
4. Repeat for `virtio_net`, `json`, `fb` as needed to get under the size cap.

## Definition of done

- [ ] No file we keep is over ~250 lines (excluding generated/asset data).
- [ ] Every split module is `curlee check`-clean individually.
- [ ] `make check` passes (all modules + merged kernel).
- [ ] All existing smoke gates pass unchanged: `make qemu-smoke`,
      `make qemu-net-smoke`, `make qemu-e1000-smoke` (behavior preserved).
- [ ] `kernel/kernel.curlee`'s `main` is a thin orchestrator (bringups +
      marker calls), leaving room to add the interactive chat loop next.
- [ ] Committed incrementally (one slice per commit) so regressions are
      traceable.

## Explicit non-goals (do NOT do in this task)

- Do NOT add the chat loop / serial input yet (separate task — see roadmap
  WS-A). This task only makes the codebase model-workable.
- Do NOT change driver behavior, register values, or gate assertions.
- Do NOT delete files that still feed the build; if a file becomes empty,
  remove it AND its wiring in one commit with the gate green.
