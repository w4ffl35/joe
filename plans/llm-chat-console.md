# Task: replace the hello-world one-shot boot with an interactive LLM console

**Status:** proposed GitHub issue / roadmap WS-A + WS-D1 (see
`docs/roadmap-to-end-goal.md`).
**Goal:** instead of printing "Hello World from JOE" and halting, JOE boots to
an interactive prompt the user can type into, and the system routes input to
the Curlee model and prints the response — a chat with the LLM, not a
traditional terminal.

## Phase 1 — get input into the kernel (foundation)

Today the kernel has **serial OUT only** (`putc` in `kernel/serial.curlee`)
and no keyboard driver. Nothing can send a byte IN.

- **1a. Serial input (`getc`)** in `kernel/serial.curlee`: add a `getc` /
  `serial_read_byte` that polls COM1's LSR (0x3FD) bit 0 (data-ready) then
  reads DATA (0x3F8). Mirror the existing `putc` structure exactly (same
  `port_inb`/`port_outb` builtins, same busy-wait shape). Must be a small,
  `curlee check`-clean slice.
- **1b. A line reader**: a small function that reads bytes until newline into
  a fixed buffer (no malloc — a static `[Int; N]` buffer). Echo each char
  back via `putc` so the user sees what they type.

## Phase 2 — the interactive loop (replaces `curlee_halt()`)

In `kernel.curlee`, after the existing bringups, replace the terminal
`curlee_halt()` with a loop:

```
print prompt "JOE> "
loop:
  read a line
  if empty: continue
  handle the line (start with built-ins: "help", "hello", "clear", ...)
  print "JOE> " again
```

Built-ins first; a free-text line is the trigger for Phase 3.

## Phase 3 — talk to the model (host-side first, then in-OS)

The model is NOT in the OS yet. Two sub-paths:

- **3a (host-side, achievable now):** the kernel's line is shipped to the
  dev host — over serial (the host reads the kernel's serial output and
  injects the model's reply back on the serial input line) or over the
  existing virtio-net path (the kernel already emits net frames to a host
  stub; add a reverse channel). The host runs headlesscode/serve_lora and
  returns the model's text, which the kernel prints. This gives the *feel*
  of "the OS chats with the model" before the model is packaged.
- **3b (later, in-OS):** package the model (or reach a model server over the
  NIC) so JOE itself hosts the intelligence. Far-term; depends on WS-B
  storage/net maturity + the simplify work.

## Constraints / dependencies

- **Do this AFTER the kernel files are simplified** (roadmap WS-C /
  `plans/simplify-large-kernel-files.md`): the interactive loop must live in a
  `main` that is a thin orchestrator, not 200 inline lines. If kernel.curlee
  is still 833 lines, split it first.
- `getc` + the loop are small enough to be model-written slices — use the
  `docs/slice-task-template.md` recipe. The serial-input slice is the natural
  first one.
- The console must not break existing gates: bringups still run before the
  loop, and the smoke gates that assert serial markers still see them (the
  markers print before the prompt). QEMU smoke asserts must stay green.
- **"Clear" / display** can use the existing VGA text + fb paths once a
  keyboard path exists; serial is the primary console for now (authoritative
  per the project's own convention).

## Definition of done

- [ ] `serial.curlee` has a verified `getc` (serial input) slice committed.
- [ ] `main` boots, runs bringups, prints `JOE>`, and accepts typed input
      (echoed) via serial; built-in commands respond.
- [ ] Free-text input routes to the model (host-side 3a) and the reply prints
      on the console.
- [ ] Existing smoke gates still pass.
- [ ] The old "Hello World then halt" is gone (replaced by the prompt loop).
