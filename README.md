# JOE

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-red)

A blazing fast minimal operating system for hackers.

JOE runs one language - [CURLEE](https://github.com/w4ffl35/curlee).

JOE is a minimalist operating system that will remain thin. Do not expect it to bloat with drivers, software or other features.

JOE does NOT come with a web browser or a calculator or nearly any other software.

JOE DOES come with CURLEE, the ability to run AI inference on your own models, and some minimal tools to help you get started.

JOE also comes bundled with Qwen 3.5 9b. JOE is natively capable of running LLMs, TTS, STT and Image generation models.

YOU are respondible for wrighting your own software and drivers for JOE. If you want to use JOE, you will need to learn how to write software for it, or better yet: make your LLM write software for you.

---

## Phase 1: Bare-Metal "Hello World from JOE"

This phase builds the first bootable JOE kernel: a freestanding x86-64
kernel written in Curlee that boots and renders **"Hello World from JOE"**
to the screen, with serial output for verification.

### Architecture

- **Language**: [Curlee](https://github.com/w4ffl35/curlee) — a
  verification-first language. The kernel is written in the freestanding
  Curlee subset (Int/Bool arithmetic, `if`/`while`, `extern fn`, and
  `Phys<T>` raw memory access under `unsafe` + `cap phys.mem`).
- **Toolchain**: Curlee's `curlee build --link` emits freestanding C from the
  verified program, compiles it with `-ffreestanding -fno-builtin -nostdlib`,
  assembles its `crt0.S` boot stub, and links with its `linker.ld` into a
  bootable x86-64 multiboot2+PVH kernel ELF.
- **Boot (primary, QEMU)**: `qemu-system-x86_64 -kernel build/kernel.elf`
  boots via the PVH note (`.note.Xen`) directly into 64-bit long mode.
- **Boot (secondary, VirtualBox)**: `make iso` produces a GRUB-bootable ISO
  (`build/joeos.iso`) wrapping the same kernel ELF. GRUB's multiboot2 loader
  (which supports x86-64 ELF kernels) enters the kernel in long mode.
- **Display**: the kernel renders "Hello World from JOE" to the VGA text
  buffer (`Phys<U16>` writes at `0xB8000`, attribute 0x0F), after programming
  VGA text mode 3 (`vga_setup.c`). Under QEMU this renders perfectly.
  A framebuffer renderer (`fb.c`, 5x7 glyphs as 32bpp pixels) is included and
  is selected when a real linear framebuffer is available; it is not yet
  reachable because the multiboot2 framebuffer address (passed in registers
  only for 32-bit entries) is not exposed to Curlee's freestanding codegen.
- **VirtualBox display note (known limitation)**: VirtualBox's VGA emulation
  does not reliably present a bare-metal kernel's text plane or framebuffer
  after a GRUB handoff — attempts with text mode, mode-3 reset, gfxterm, and
  the `0xFD000000` framebuffer all showed garbled/black output. The kernel
  itself is correct (QEMU renders it perfectly).
- **Verification (authoritative)**: every boot — QEMU `-kernel`, GRUB ISO
  under QEMU, and VirtualBox — emits `Hello World from JOE!` on COM1,
  captured to a host serial log. This is the deterministic, machine-checked
  acceptance gate (`make qemu-smoke`).

### Project layout

```
Makefile                 # build: merged kernel.elf, iso; run: qemu; verify gates
scripts/
  find-curlee.sh         # locate the curlee compiler (CURLEE/CURLEE_ROOT/PATH)
  build_iso.sh           # grub-mkrescue GRUB ISO (VirtualBox path)
  vbox-setup.sh          # automated VirtualBox VM creation + boot (VBoxManage)
  build-kernel.sh        # concat pure modules + kernel.curlee -> single-TU merged file
kernel/
  kernel.curlee          # Phase 2 entry: externs, render_frame scene, main
  canvas.curlee          # pure verified color/geometry math (VM-tested)
  glyphs.curlee          # pure 5x7 glyph math + text layout (VM-tested)
  assets.curlee          # pure blit-fit + frame-ring math (VM-tested)
  canvas_test.curlee     # VM-runnable assertions over the pure modules
  pack.curlee            # pure cell/pixel helpers (VM-testable, verified)
  boot.S                 # multiboot2 64-bit entry (GRUB ISO path)
  putc_driver.c          # COM1 serial override of weak curlee_putc
  vga_setup.c            # VGA text-mode-3 programming (QEMU display path)
  fb.c                   # linear-FB software blitter (pixel/fill/line/text/blit)
```

### Prerequisites

- The **Curlee compiler** (built from `~/Projects/curlee`; the Makefile
  auto-detects it via `scripts/find-curlee.sh`). Set `CURLEE` or
  `CURLEE_ROOT` if it's elsewhere.
- `cc`, `ld`, `objdump`, `nm`, `readelf` (binutils + a C compiler).
- `qemu-system-x86_64` (for boot testing).
- `grub-mkrescue` (grub-pc-bin / grub2-common) — only for `make iso`.

### Build & verify

```sh
make kernel     # -> build/kernel.elf  (QEMU path)
make iso        # -> build/joeos.iso   (VirtualBox path)
make verify     # static gates: check, packer test, canvas-run, ELF entry,
                # _start, curlee_main, PVH note
make qemu-smoke # dynamic gate: boot kernel.elf, assert serial log contains
                # "Hello World from JOE"
make qemu-fb-smoke   # dynamic gate: boot FB ISO, assert serial "FB: 1"
make qemu-loop-smoke # dynamic gate: boot FB ISO, assert the 60 FPS loop ran
                     # frames FR:0..FR:2+ deterministically
```

### Run

```sh
# QEMU (primary development tool) — the kernel's boot output prints LIVE to
# your terminal ("Hello World from JOE!"), no GUI needed. Ctrl-A X to quit.
make qemu-serial

# QEMU with a real GUI window (like VirtualBox) when a display server is
# available; serial goes to build/serial.log
make qemu-display

# QEMU exposing a VNC server (127.0.0.1:5901) — connect with any VNC client
# to see the rendered VGA text; serial goes to build/serial.log
make qemu-gui

# QEMU headless — serial output to build/serial.log (CI/automation)
make qemu

# Boot the ISO under QEMU (sanity for the VirtualBox path)
make qemu-iso

# VirtualBox — fully automated (creates VM, attaches ISO, enables serial,
# starts it):
make iso                          # -> build/joeos.iso
bash scripts/vbox-setup.sh        # creates/registers/starts the "JOE" VM
# Optional flags:
#   --iso <path>    use a different ISO (e.g. on an external drive)
#   --name <vm>     VM name (default JOE)
#   --headless      run without a GUI window (serial log still captures output)
#
# The VM is configured with 64 MiB RAM / 1 vCPU, DVD-first boot, no disk/network,
# and COM1 -> a host serial log (build/vbox-serial.log, or next to the ISO).
# If the host's KVM module holds the virtualization extension, the script
# temporarily unloads it, starts the VM, and reloads KVM afterward.
#
# Manual steps (equivalent):
#   1. New VM -> Linux / Other 64-bit
#   2. Attach build/joeos.iso as a CD/DVD drive
#   3. Boot (it is bootable)
#   "Hello World from JOE" appears on screen; serial output (if a serial
#   port is configured) carries the same text for host logs.
```

### Recommended development loop

Use **QEMU** to test the machine as you build it — it's what Curlee's own CI
uses, and its serial output is the kernel's console:

```sh
make qemu-serial # kernel prints "Hello World from JOE!" live to the
                 # terminal — the primary dev loop (Ctrl-A X to quit)
make verify      # static verification gates
make qemu-smoke  # automated boot + serial assertion
```

Use **VirtualBox only for packaging validation** (that the ISO is bootable
and the kernel runs), since its VGA emulation does not reliably present a
bare-metal kernel's display after a GRUB handoff — the serial log is the
proof there: `bash scripts/vbox-setup.sh --headless` then check
`vbox-serial.log` for `Hello World from JOE!`.

### Verification gates (acceptance criteria)

| Gate | Command | Pass condition |
|------|---------|----------------|
| Verify | `make verify` | `curlee check` on all pure modules + merged kernel; `curlee run` packer → 0; `curlee run` canvas_test → 0; ELF entry set; `_start` + `curlee_main` present; `.note.Xen` PVH present |
| Renderer math | `make canvas-run` | `curlee run kernel/canvas_test.curlee` → 0 (color/geometry/glyph/asset/tool-queue assertions) |
| Boot (QEMU) | `make qemu-smoke` | kernel.elf boots under QEMU; serial log contains `Hello World from JOE!` |
| Boot (ISO/GRUB) | `make qemu-iso` | ISO boots under QEMU via GRUB; serial log contains `Hello World from JOE!` |
| Framebuffer (ISO) | `make qemu-fb-smoke` | FB-mode ISO boots; serial log contains `FB: 1` (multiboot2 framebuffer tag parsed, `fb_ready()==1`) |
| 60 FPS loop (ISO) | `make qemu-loop-smoke` | FB-mode ISO boots; serial log contains `FR:0`, `FR:1`, `FR:2` then `FB: 1` (the deterministic loop rendered >1 frame) |

All gates pass on this branch (QEMU serial: `Hello World from JOE!`, GRUB
ISO boots, ELF entry + symbols + PVH note verified).

---

## Phase 2: Agentic Framebuffer OS (software renderer)

Phase 2 refactors the static test kernel into an AI-native, single-address-space
OS with a freestanding software renderer targeting the linear framebuffer.

### Architecture

- **Two layers**: Curlee computes geometry/intent (pure, Z3-verified, VM-tested);
  the C driver ([`kernel/fb.c`](kernel/fb.c)) owns mutable state and executes the
  pixel loops. Curlee has no assignment and a small verifier fragment, so
  imperative blitting lives in C while provable math lives in Curlee.
- **Single-TU merge**: the freestanding codegen crashes on `import`, so
  [`scripts/build-kernel.sh`](scripts/build-kernel.sh) concatenates the pure
  modules + `kernel.curlee` into one self-contained translation unit.
- **Pure modules** (all VM-testable via `make canvas-run`):
  - [`kernel/canvas.curlee`](kernel/canvas.curlee) — color packing/channels,
    rect containment/cover, clamp, clip, alpha blend
  - [`kernel/glyphs.curlee`](kernel/glyphs.curlee) — 5x7 glyph pixel test +
    text layout math
  - [`kernel/assets.curlee`](kernel/assets.curlee) — blit-fit OOB gate + frame
    ring-buffer slot math, static asset-region geometry, runtime fit gates
    (rect/line/char/asset), frame-ring geometry (Phase 2c)
- **Blitter** ([`kernel/fb.c`](kernel/fb.c)): `fb_clear`, `fb_pixel`,
  `fb_fill_rect`, `fb_line`, `fb_draw_char_color`, `fb_blit_asset`,
  `fb_present`, plus the Phase 2b 60 FPS loop + tool ring
  (`fb_loop_init`/`fb_loop_frame`/`fb_run_loop`/`fb_tool_enqueue`/...).
  All bounds-checked; the tool ring + asset region + frame ring are
  fixed-slot static arrays (no malloc). Phase 2c: `fb_present()` performs a
  real back-buffer flip on the GRUB framebuffer path (serial `RING: 1`).
- **Declarative scene**: `render_frame(frame)` in
  [`kernel/kernel.curlee`](kernel/kernel.curlee) describes one frame (colors
  bound to `let`s — the verifier rejects calls as call arguments); every
  primitive is gated by a verified pure gate reading the runtime FB size from
  `fb_get_width`/`fb_get_height` (out-of-bounds primitives are skipped);
  Curlee's `main` drives the fuel-bounded loop when `fb_ready()` is 1, else
  falls back to VGA text + serial.

### Phase 2 roadmap

| Phase | Scope | Status |
|-------|-------|--------|
| 2a | Software renderer: blitter primitives, text, bitmap/frame blit, pure verified math modules | ✅ done |
| 2b | Kernel tool API & 60 FPS event loop — Curlee `main` drives a fuel-bounded while-loop; `fb.c` owns the frame counter + fixed-slot tool ring; `make qemu-loop-smoke` asserts frames FR:0..FR:2+ | ✅ done |
| 2c | Memory & asset management contracts (ring-buffer hardening, static buffers) | ✅ done — static asset region (128x128) + 2-slot 640x480 frame ring (no malloc); every blit/fill/line/text path gated by verified pure gates (OOB primitives skipped); `fb_present()` real flip on the GRUB FB path (serial `RING: 1`); PVH path compiles the ring out (`JOE_PVH_BOOT`) for QEMU's PVH loader limit; canvas_test §14–17 VM-asserts all geometry |
| 2d | LLM bridge (VirtIO-net/TCP + JSON) — host-side llama.cpp | planned |
| 2e | Framebuffer address plumbing — 32-bit multiboot2 entry + framebuffer request tag; serial `FB: 1` proves `fb_ready()==1` (`make qemu-fb-smoke`) | ✅ done |

See [`docs/phase2-architecture.md`](docs/phase2-architecture.md) for the full
design, the Curlee verifier-fragment constraints discovered, and the Curlee
codegen import fix as a follow-up work item.

### Design notes

- **Single-address space, Ring 0**: `Phys<T>` writes are direct volatile
  stores; no MMU setup, no privilege transitions.
- **Deterministic verification**: every `phys<U32>(0xADDR)` address is a
  constant literal (Curlee rule), the packer (`pack.curlee`) is pure
  `Int -> Int`, and the full check pipeline (lex → parse → resolve →
  type-check → verify) runs before any code is emitted (`no proof, no build`).
- **Minimal footprint**: no libc, no malloc, no String/Vec (rejected by the
  freestanding target), no drivers beyond the VGA buffer + COM1 serial hook.

## Installation

TODO: Write installation instructions.

## License

See [LICENSE](LICENSE) for license rights and limitations (GPL 3.0).
