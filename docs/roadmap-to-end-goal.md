# Roadmap: from "Hello World" to "an OS with a packaged model that writes drivers on the fly"

*Written 2026-09-05. Plain-English gap analysis grounded in the current repo
state. Companion docs: `joeos-llm-agent-plain-english.md` (model/harness),
`slice-task-template.md` (how the model reliably writes code),
`session-handoff-2026-09-05.md` (technical session log).*

---

## The end goal (restated)

JOE boots with our fine-tuned Curlee-writing model available to it, and can
**write device drivers on the fly** — either autonomously (detects hardware,
writes the driver) or interactively (the user prompts it through a chat
interface inside the OS). "On the fly" means the OS can *generate and load new
driver code at runtime*, not just ship drivers compiled in.

## Where we are today (verified 2026-09-05)

- **Kernel:** boots under QEMU, renders "Hello World from JOE" to VGA +
  serial, runs a fixed sequence of hardware bringups (e1000 probe, virtio-net,
  TCP demo, framebuffer frames, LLM-net markers), then **halts**
  (`curlee_halt()`). One-shot; **no input, no loop, no user interaction.**
- **Drivers in the kernel (compiled in):** serial OUT (COM1, `putc` only —
  **no serial IN**), VGA text, framebuffer/canvas/glyphs, virtio-net (+TCP
  stack), e1000 probe. **No storage, no keyboard, no serial-input.**
- **Model/harness (host-side, NOT in the OS):** the Curlee LoRA (n1235) +
  headlesscode harness run on the dev machine. Proven it can write small,
  verified Curlee modules (7 virtio-blk pure modules landed on master).
- **virtio-blk:** only the *pure helper* foundation exists (helpers, layout,
  queue geometry, request types, bounds, reqbuf, alloc). No actual PCI probe,
  no virtqueue MMIO driver, no read path, no disk image, no smoke gate.

---

## The gap, broken into concrete workstreams

### WS-A. Make the kernel interactive (a prompt, not a one-shot hello)

**Why first:** "chat with the LLM instead of a traditional terminal" needs an
input path + a loop + an output path. Today there is no way to get a byte
*into* the kernel.

- **A1 — Serial input (`getc`)**: add COM1 receive support to `serial.curlee`
  (read LSR bit 0 = data-ready, then DATA 0x3F8). Pure-ish, small, mirrors the
  existing `putc`. This is the first "input driver."
- **A2 — A command/chat loop in `main`**: instead of `curlee_halt()`, loop:
  print a prompt → read a line from serial → echo/handle it. Needs a line
  buffer + the loop. This replaces the "traditional terminal" with an
  interactive console.
- **A3 — (optional, later) PS/2 keyboard + VGA text echo** for real terminal
  use without serial.

**Size note:** kernel.curlee is 833 lines and `main` is already huge — A2 may
require splitting kernel.curlee first (see WS-C) because the model can't edit
large files.

### WS-B. Finish a real storage driver (virtio-blk) — the "driver on the fly" demo target

The pure modules exist. What's missing is the actual driver + proof:

- **B1 — PCI probe for the virtio-blk device** (mirror virtio_net's probe):
  find vendor/device, set PCI command, read BAR.
- **B2 — Legacy virtqueue setup** (one queue): queue_select, queue_num,
  queue_pfn, the 3-page ring (the `vblk_*` queue/alloc helpers already encode
  the geometry).
- **B3 — The read request path**: build a 16-byte request header (type=IN,
  sector=LBA), place it + a data buffer in descriptors, kick the queue, poll
  the used ring, return the bytes.
- **B4 — A disk image + QEMU smoke gate**: put a known pattern on a virtio-blk
  drive, boot, read it, assert via serial marker (mirror qemu-net-smoke).
- **B5 — Wire into the merged kernel TU** (this is the hard freestanding part:
  Phys/port I/O + statics; the model can't write this whole thing in one
  session — it must be slice-driven like the pure modules, with a human or
  orchestrator chaining the slices).

**Why it matters:** virtio-blk is the cleanest "the model wrote a real,
working driver, proven under QEMU" milestone — the template for on-the-fly
driver generation.

### WS-C. Simplify / split the large kernel files

**Why:** the model cannot edit 800-1000 line files (net_stack 984,
virtio_net 892, kernel.curlee 833, json 692, fb 688). It can only reliably
write NEW small files. So the codebase must be shaped so the model can work
on it — split the files we intend to keep into small (<~150 line) modules,
each with a clear single responsibility. (See the separate simplify task —
item 3b — which scopes this and ties it to "only rewrite files we keep".)

### WS-D. Replace hello-world with the LLM-chat experience

Once A (input+loop) exists, the console's job becomes talking to the model:
- **D1 (host-side, now-ish):** a dev-loop where headlesscode/the model is
  reachable over the kernel's serial/net and can be prompted (this is what the
  net/LLM markers already gesture at — the kernel currently *emits* net frames
  to a host stub).
- **D2 (far-term, in-OS):** actually *packaging* the model (or a model server
  the OS reaches over its NIC) so the OS itself hosts the intelligence.

### WS-E. The on-the-fly driver-writing loop (the actual end-goal mechanism)

Even with a packaged model, "writes drivers on the fly" needs the
**slice-chaining orchestrator**: detect hardware → architect splits the
driver into slices → code worker writes slice 1 → verify (curlee check) →
review → next slice → assemble → load. We proved each individual slice works;
the missing piece is a *reliable automatic chain* (the harness's orchestrator
does this for cloud models; for the local 9B it needs the human/architect
slice discipline + per-slice dispatch we developed).

---

## Prioritized order (what to do next)

1. **WS-C first (scoped simplify)**: split kernel.curlee + the big drivers
   into small modules the model can work with. This unblocks everything else
   (A2 needs a smaller main; B5 needs the model to touch kernel files).
   → This is the "simplify large files" GitHub task you asked for.
2. **WS-A1**: serial input (`getc`) — the first input capability, small,
   model-writable.
3. **WS-A2 + WS-D1**: an interactive console (host talks to the model over
   serial/net) — the "chat with the LLM instead of a traditional terminal"
   milestone, achievable *before* the model is packaged in-OS (the model lives
   host-side and the OS console is its terminal).
4. **WS-B**: drive virtio-blk to a real read + QEMU gate, slice by slice.
5. **WS-E + WS-D2** (far-term): in-OS model + automatic driver-writing loop.

---

## Definition of "done" for the end goal (acceptance)

- JOE boots to an interactive prompt (serial and/or VGA+keyboard).
- A user can type a request; the system routes it to the model and displays
  the response (model may be host-side via NIC first, in-OS later).
- The system can generate a *new* device driver for a detected device and
  load/run it — demonstrated end-to-end at least once (virtio-blk is the
  target).
- The driver-generation pipeline (architect-split → slice → verify → review →
  assemble) runs without a human hand-holding each slice.
