# JOE

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-red)
![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)

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
