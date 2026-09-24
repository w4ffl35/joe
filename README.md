# JOE

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-red)
[![build](https://img.shields.io/github/actions/workflow/status/w4ffl35/joe/ci.yml?branch=master&label=build)](https://github.com/w4ffl35/joe/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/w4ffl35/joe?label=release)](https://github.com/w4ffl35/joe/releases)
![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)
[![Discord](https://img.shields.io/badge/Discord-Join%20Chat-5865F2?logo=discord&logoColor=white)](https://discord.gg/7254Hkzc4T)

A from-scratch x86-64 kernel whose entire driver layer is written in
[Curlee](https://github.com/w4ffl35/curlee), a verification-first systems
language. Every module passes formal verification (`curlee check`, backed by
Z3) before the kernel is built. No proof, no build.

## What is this

JOE is an experimental, pre-alpha hobby OS kernel. Its driver layer — serial,
VGA setup, VBE probe, multiboot2 parsing, framebuffer blitter, VirtIO-net,
TCP/IP, and the JSON parser — is 100% Curlee after a full migration from C.
It boots under QEMU and VirtualBox, renders a software framebuffer, and runs
a real TCP/IP stack that performs an HTTP round-trip against a host-side
server. The kernel does not run AI inference, bundle any model, or provide
LLM/TTS/STT/image-generation capabilities. Its "LLM bridge" is a
network/JSON/tool-queue exercise: the model (or a deterministic stub) lives
on the host, never in the kernel.

What is unusual here is the toolchain: a formally verified language is
generating the kernel's drivers. That is the point of the project, and it is
compelling on its own.

## Quick start

Prerequisites: the Curlee compiler (`scripts/find-curlee.sh` auto-detects it;
override with `CURLEE` or `CURLEE_ROOT`), `cc`/`ld`/binutils, and
`qemu-system-x86_64`. `grub-mkrescue` is needed only for `make iso`.

```sh
make kernel          # verify + codegen + link -> build/kernel.elf (PVH path)
make verify          # static gates: curlee check/run, codegen harnesses, ELF checks
make qemu-smoke      # boot the PVH kernel, assert "Hello World from JOE" on serial
make iso             # GRUB-bootable ISO -> build/joeos.iso (VirtualBox path)
make qemu-fb-smoke   # boot the framebuffer ISO, assert FB: 1 and RING: 1 on serial
```

All of the above were run on a clean checkout and pass. For the live dev
loop, `make qemu-serial` prints boot output to the terminal (Ctrl-A X to
quit). VirtualBox is exercised via `bash scripts/vbox-setup.sh --headless`
after `make iso`.

### Bundling an application

By default the kernel runs its own built-in demo scene. An external
application can take over instead:

```sh
make qemu-app-smoke APP=apps/hello   # boot with apps/hello bundled
```

`APP=<dir>` also works with `make iso-fb` (an app that draws to the
framebuffer, unlike `apps/hello`, needs this GRUB path, not the PVH
`-kernel` path `qemu-app-smoke` boots): `make iso-fb APP=apps/hello`
bundles it into `build/joeos-fb.iso` the same way.

`APP=<dir>` is shorthand for `APP_MODULES=<dir>/app.curlee` (the
one-file-per-app convention `apps/hello` and `apps/noop` follow).
`APP_MODULES="path1 path2 …"` is the lower-level knob for a multi-file
app — a space-separated list of `.curlee` files, which may be absolute
paths outside this tree (`scripts/build-kernel.sh` appends them after
the kernel's own modules; a missing file is a build error naming it).
`APP_DATA="path1 path2 …"` names data files an app reads with `ingest`
(for example NumPy `.npy` arrays): `scripts/build-kernel.sh` copies them
next to `build/kernel-merged.curlee`, because an `ingest` path is relative
to the file that declares it and may not leave that directory. The files are
prerequisites of the merged file, so changing one rebuilds the kernel; a
missing file, or two files with one name, is a build error
(`make app-data-test`).

An application implements two functions:

```
fn app_init(pm: cap phys.mem) -> Unit    // called once, before the loop
fn app_frame(pm: cap phys.mem) -> Int    // called each frame; 0 = continue,
                                          // non-zero = this was the last frame
```

`main` calls `app_init` once, then `app_frame` in a loop until it
returns non-zero, then falls through to the kernel's normal halt path
(unchanged either way) — the built-in demo never runs before `app_init`
or after the app's last frame, so nothing overwrites what the app left
on screen. Every app module is concatenated into the same single
translation unit as the rest of the kernel (see `scripts/build-kernel.sh`
and `kernel/kernel.curlee`'s `main`), so an app can call any function
the kernel already defines by name — `fb.curlee`'s blitter
(`fb_pixel`/`fb_fill_rect`/`fb_blit_asset`/`fb_present`/...) and
`serial.curlee`'s `putc` included. Reaching either requires the
`pm: cap phys.mem` capability; an app gets it the same way `main` and
every kernel function already do — as a plain parameter, threaded down
from the entry point rather than held globally. Curlee has no weak
symbols, so a real `app_init`/`app_frame` definition (an app's own, or
the in-tree `apps/noop` default) is always present; a build-time
`--define` tells `main` whether to actually run it. See `apps/hello/`
for a minimal, fully worked example.

### Frame pacing timer

The kernel starts a polled clock on PIT channel 0 before `app_init`
(`kernel/timer.curlee`; no interrupts). An application calls:

```
fn timer_now(pm: cap phys.mem) -> Int
    // microseconds since the kernel started the clock
fn timer_wait_until(pm: cap phys.mem, target: Int) -> Int
    // spins until timer_now() >= target and returns that reading
```

- **Unit and cost.** Microseconds. The counter itself ticks every 838 ns
  (1193182 Hz); a microsecond unit keeps applications independent of that
  chip. A call is a latch and two port reads plus one multiply, with no
  division, on the 64-bit and on the 32-bit GRUB build alike.
- **Non-decreasing.** `timer_now()` never returns less than the call before
  it. Module-level state is opaque to the verifier, so the contract sits on
  the pure update (`timer_us_after` in `kernel/timer_math.curlee`:
  `ensures result >= us`, and at most 54925 us per poll), which is the only
  place the clock advances. `timer_wait_until` has `ensures result >= target`.
- **Poll at least once every 54.9 ms.** The counter is 16 bits and wraps
  every 65536 ticks (54.9 ms), and one counter cannot say how many times it
  wrapped. A longer gap between two calls is not detected: whole periods are
  lost (a 60 ms gap counts as 5 ms), the clock stays non-decreasing, and it
  runs late by the lost time from then on. `timer_wait_until` polls back to
  back, so it stays inside the limit. An application that calls
  `timer_now` once per 60 Hz frame is inside it while no frame takes longer
  than 54.9 ms. The same loss happens if the host stops the virtual CPU for
  that long.
- **Pacing.** Take each frame's deadline from a fixed start
  (`start + n * 1000000 / 60`), not from the end of the previous frame, so
  one late frame does not push the rest back. `apps/pace` does this.

`make qemu-pace-smoke` bundles `apps/pace` (600 frames at 60 Hz, one
`PACE: <nnn>` line per frame), boots it under KVM and under TCG on both the
PVH `-kernel` path and the GRUB ISO, timestamps each line as it reaches the
host, and requires 60 frames per second within 1% over the 10 seconds. The
KVM runs are skipped, with a message, when `/dev/kvm` is not accessible.
`make timer-run` runs the clock arithmetic on the VM.

### Keyboard

`kernel/keyboard.curlee` reads the PS/2 keyboard by polling: nothing blocks
and there are no interrupts, so an application can poll once per frame. It
calls:

```
fn kbd_init(pm: cap phys.mem) -> Unit
    // once: drops queued bytes, clears the state
fn kbd_poll(pm: cap phys.mem) -> Int
    // reads up to 16 queued bytes, returns how many
fn kbd_state() -> Int
    // the keys held now, as the bits below
fn kbd_take_seen() -> Int
    // the keys pressed since the last call; clears them
```

- **Bits of `kbd_state()`.** up 1, down 2, left 4, right 8, Z 16, X 32,
  C 64, Enter 128, Escape 256 (`KBD_UP` to `KBD_ESC`). They are raw keys;
  the driver does not map them to game actions, and its contracts state the
  bit of each scancode (`kernel/keyboard_keys.curlee`).
- **Scancodes.** The driver reads scancode set 1, make and break codes,
  which a PC's controller produces from the keyboard's own codes. The
  arrows are read both with the `0xE0` prefix a PC keyboard sends and
  without it, which is what the numeric keypad sends with NumLock on, and
  keypad Enter counts as Enter. A code after `0xE0` is looked up as an
  extended key only, so `0xE0 0x2E` (Volume Down) is not read as C
  (`0x2E`). Bytes that are no listed key change nothing.
- **Mouse and error bytes.** A byte the controller flags as coming from the
  mouse port (status bit 5) or as a timeout or parity error (bits 6 and 7)
  is read, to clear it, and dropped.
- **Bound.** One `kbd_poll` reads at most 16 bytes, which is how many
  QEMU's keyboard queues. What is left waits in the controller for the next
  call, so no break code is lost.
- **Quick taps.** A key pressed and released between two polls never shows
  in `kbd_state()`. `kbd_take_seen()` returns it, once.
- **What it relies on.** The driver does not configure the controller. It
  expects the BIOS or GRUB to have left the keyboard enabled and its bytes
  translated to scancode set 1, as SeaBIOS does under QEMU. It has been run
  under QEMU only, not on hardware.

`make qemu-kbd-smoke` bundles `apps/keys`, which prints `KEYS: <hhh>` (the
state, in hex) whenever the held keys change and `TAP: <hhh>` for keys
tapped within one poll. It boots the PVH `-kernel` image and the text ISO,
under TCG and, where `/dev/kvm` is accessible, KVM, and sends key events
through QMP: a held key, two keys, the arrows with and without the prefix,
Enter, Escape, Volume Down, a tap, and a full 16-byte queue. `make
keyboard-run` runs the decoding on the VM, including a sweep of all 256
bytes.

## What it does today

Everything below is proven by the serial markers the project's own smoke
gates assert. Serial (COM1) is the authoritative console.

- **Boots and halts deterministically.** Every boot path emits
  `Hello World from JOE!` on serial (`make qemu-smoke`).
- **Software framebuffer renderer in Curlee** (`kernel/fb.curlee`): pixel,
  fill-rect, line, blit, and glyph-text primitives, a 60 FPS event loop, and
  a back-buffer ring flip. Markers: `FB: 1` (framebuffer ready), `FR:0`..`FR:3`
  (loop frames), `RING: 1` (real flip) — asserted in order by
  `make qemu-loop-smoke`. Without a framebuffer it falls back to the VGA
  text buffer.
- **Bochs VBE probe** (`kernel/vbe.curlee`) finds a linear framebuffer on
  the PVH path (`make qemu-pvh-fb-smoke` asserts `FB: 1`).
- **VirtIO-net driver in Curlee** (`kernel/virtio_net.curlee`): PCI probe,
  virtqueue setup, RX/TX rings. Markers `NET: 1` (found) → `NET: 2` (ready)
  → `NET: 3` (link up), then `RX: <len>` (first frame) — asserted by
  `make qemu-net-smoke` with a two-QEMU socket harness.
- **TCP/IP stack in Curlee** (`kernel/net_stack.curlee`): ARP, IPv4, TCP,
  RFC 1071/793 checksums, HTTP Content-Length framing. Pure Curlee and
  VM-verified (`make net-stack-run`, `make net-stack-codegen-run`).
- **LLM-bridge round-trip:** the kernel POSTs over its own stack to a
  host-side HTTP server, parses the JSON tool-call envelope
  (`kernel/json.curlee`), enqueues the tool call, and acks. Markers
  `ARP: 1`, `TCP: 1`, `SND: 36`, `RCV: 36`, `JSON: 1`, `TOOL: 2`, `LLM: 1`
  are asserted in order by `make qemu-llm-smoke`. The default server is the
  deterministic `scripts/llm_stub_server.py` (returns the fixed 36-byte
  envelope `{"tool":"frame_tick","args":[0,1,2]}`); pointing it at a real
  llama.cpp server is documented but intentionally not part of the
  deterministic gate. The gate needs host port 8080 free.
- **Polled PS/2 keyboard** (`kernel/keyboard.curlee`): `kbd_state()` is a
  bit field of nine keys (arrows, Z, X, C, Enter, Escape); `apps/keys` echoes
  it on serial and `make qemu-kbd-smoke` drives QEMU's keyboard through QMP.
- **Polled frame clock** (`kernel/timer.curlee`): PIT channel 0,
  microsecond `timer_now`/`timer_wait_until`, no interrupts; `apps/pace`
  runs 600 frames at 60 Hz on it and `make qemu-pace-smoke` measures the
  rate from the host under KVM and TCG.
- **Verification model:** pure modules are VM-runnable (`make canvas-run`,
  `make net-stack-run`); freestanding paths are proven by host-side codegen
  harnesses (`json-codegen-run`, `net-stack-codegen-run`, `mb2-codegen-run`);
  `make verify` runs the whole chain plus ELF/symbol/PVH-note checks.

**C-to-Curlee migration is complete.** `ls kernel/*.c` finds nothing, and
`make c-boundary` (`scripts/check-c-boundary.sh`) reports "zero C files; no
grandfathered files". `kernel/net.h` survives only as the C-visible contract
documentation for the network API. The live count is tracked in the
[C-to-Curlee Migration](https://github.com/w4ffl35/joeos/milestone/1)
milestone.

## Architecture

- **Two boot paths.** *PVH*: `qemu -kernel build/kernel.elf` boots 64-bit
  directly via the ELF's PVH note (crt0.S); the VBE probe supplies the
  framebuffer. The PVH machine exposes no legacy PCI, so no NIC here.
  *GRUB/multiboot2*: the ISO path (VirtualBox, and QEMU for the gate tests);
  boot.S captures the multiboot2 info pointer and SeaBIOS's legacy PCI is
  where VirtIO-net actually runs. This is also where the framebuffer flip
  runs at full geometry.
- **Why both:** PVH is the fast, scriptable dev loop. The ISO path is the
  VirtualBox target and the only place the NIC is reachable.
- **One merged Curlee translation unit.** `scripts/build-kernel.sh`
  concatenates the modules (the freestanding codegen rejects imports). Pure
  math lives in VM-runnable modules (`canvas`/`glyphs`/`assets`/`pack`/
  `json`/`net_stack`) verified with `curlee check`; drivers use Curlee
  statics, `addr_of` getters, `phys_read`/`phys_write` builtins, and
  `Phys<T>` under `cap phys.mem`.
- **C/Curlee split rationale:** [`docs/c-boundary-policy.md`](docs/c-boundary-policy.md).
  Its one-sentence rule — no logic in C. Today there is no C at all in
  `kernel/`.
- **Design constraints:** single address space, Ring 0; no libc, no malloc,
  no MMU setup.

## Status / roadmap

This only runs under QEMU (primary) and VirtualBox (secondary). **No real
hardware has been tested** — see the
[Bare-Metal Readiness](https://github.com/w4ffl35/joeos/milestone/2)
milestone.

Limitations, stated plainly:

- No persistent storage: there is no disk driver (virtio-blk is an open item).
- Networking works only under a hypervisor's paravirtualized virtio-net.
  There is no real NIC driver.
- VirtualBox's VGA emulation garbles the text plane and framebuffer after
  the GRUB handoff; serial is the authoritative console there.
- The LLM smoke gate needs host port 8080 free (see above).

Future work is tracked in three milestones:
[C-to-Curlee Migration](https://github.com/w4ffl35/joeos/milestone/1)
(closed out), [Bare-Metal Readiness](https://github.com/w4ffl35/joeos/milestone/2)
(real-hardware validation), and
[Native Inference (Edge)](https://github.com/w4ffl35/joeos/milestone/3)
(open investigation).

## Relationship to Curlee

JOE is both a proving ground for and a consumer of
[w4ffl35/curlee](https://github.com/w4ffl35/curlee). Kernel work keeps
surfacing language gaps — extern statics, `addr_of`, runtime-address
`phys_read`/`phys_write` builtins, and per-build static sizing via `--define`
all landed because this kernel needed them. See the Curlee repo for the
language itself.

## License

GPL-3.0 — see [LICENSE](LICENSE).
