# JOE OS — Phase 2e-2: 32-bit multiboot2 entry → framebuffer display (report)

Status: **COMPLETE — `make qemu-fb-smoke` PASSES** (serial `FB: 1` proves
`fb_ready()==1` end-to-end). All other gates stay green.
Parent: [`docs/phase2e-architecture.md`](docs/phase2e-architecture.md)
(§7 findings, §8 acceptance), [`docs/phase2-architecture.md`](docs/phase2-architecture.md).

---

## 1. What was done

Phase 2e delivered the renderer + blitter + guarded multiboot2 parser, but the
display path was **blocked**: GRUB's x86-64 ELF entry (ELFCLASS64, long mode)
does not pass the multiboot2 info pointer in `%ebx` and doesn't load `.data`.
Phase 2e-2 implements the documented fix — a **32-bit multiboot2 entry** — and
flips the `qemu-fb-smoke` gate green.

### 1.1 Design change from the plan (empirical)

The original plan proposed a "mixed-class" kernel: a 32-bit boot.S trampoline
that transitions to long mode and jumps to a 64-bit `curlee_main`, keeping the
codegen C 64-bit. Toolchain verification proved this **impossible**:

- GNU `ld` refuses to link ELFCLASS32 + ELFCLASS64 objects into one image
  (`ld: i386 architecture of input file ... is incompatible`).
- Compiling the codegen C with `-m32` emits `__divdi3`/`__muldi3` libgcc
  helpers for the 64-bit `Int` math (not in the freestanding runtime).

**Chosen design:** the GRUB-path kernel (`kernel-grub.elf`) is a **fully 32-bit
ELF** (`ELFCLASS32`): `boot.S` assembled `--32`, all C objects `-m32`, linked
with `ld -m elf_i386`. This is exactly the case where the multiboot2 spec
**guarantees** `%ebx` = boot info pointer in 32-bit protected mode. The 64-bit
PVH/QEMU path (`kernel.elf` via `crt0.S`) is completely unchanged.

### 1.2 Files changed/added

| File | Change |
|------|--------|
| `kernel/boot.S` | Rewritten: `as --32`, `.code32` entry stores `%ebx`→`mb2_info_addr`, zeroes `.bss`, sets stack, `call curlee_main`; added **framebuffer request tag (type 5)** 640x480x32 + 8-byte-aligned end tag |
| `kernel/libgcc32.c` | NEW: freestanding `__muldi3`/`__divdi3`/`__moddi3`/`__udivdi3`/`__umoddi3`/`__udivmoddi4`/`__negdi2`/shift helpers (32-bit ops only) |
| `scripts/linker-grub.ld` | NEW: ELF32 output, `__bss_start/__bss_end/__stack_top`, `KEEP(.multiboot2)` |
| `Makefile` | `kernel-grub.elf`: `-m32` for all objects, `as --32`, `ld -m elf_i386`, link `libgcc32.o`; PVH path untouched |
| `kernel/mb2.c` | Fixed framebuffer tag struct (spec: **u32** pitch/width/height — was `u64`); guard relaxed from `< 1 MiB` to `< 4 GiB` (GRUB puts the info right after the kernel at ~0x109000); kept total_size-bounded walk |
| `kernel/fb.c` | `fb_init()` = `mb2_parse()`; removed hardcoded VBE fallback (unmapped writes fault — §7 finding 4) |
| `kernel/kernel.curlee` | Added `serial_fb_marker()` (`FB: 1`); called in `main` when `fb_ready()==1` |
| `docs/phase2e-architecture.md` | §7 conclusion + §8 acceptance updated to RESOLVED |
| `docs/phase2e-2-report.md` | This report |

## 2. Empirical bring-up findings (serial-instrumented against QEMU)

1. **The 32-bit entry works**: `%ebx` captured a genuine info pointer
   (`mb2_info_addr = 0x109A78`), proving GRUB enters ELFCLASS32 kernels in
   32-bit protected mode per spec.
2. **The `< 1 MiB` guard was too strict**: GRUB places the info structure right
   after the kernel image (~0x109000), not below 1 MiB. Relaxed to `< 4 GiB`;
   the `total_size` bound keeps every walk safe.
3. **The framebuffer tag struct was wrong**: pitch/width/height are `u32` per
   spec; the original `u64` fields shifted `bpp` out of the tag (which is only
   0x20 = 32 bytes), so `bpp != 32` and the tag was rejected.
4. **gfxterm alone does not set a linear framebuffer**: without a request tag,
   GRUB's info reported VGA text mode (`a:0xB8000, p:160, w:80, h:25, b:16`).
   Adding the **multiboot2 framebuffer request tag (type 5)** made GRUB set
   640x480x32 before entry: `a:0xFD000000, p:0xA00, w:640, h:480, b:32`.
5. **Header tags must be 8-byte aligned**: the 20-byte fb-request tag ended at
   offset 36; the end tag needed a `.balign 8` (offset 40). Misalignment made
   GRUB fail to find the end tag and skip the kernel entirely (no output).
6. **`-m32` codegen C links with the libgcc shim**: the merged kernel's
   64-bit division/multiply resolves to `kernel/libgcc32.o`; no libgcc needed.

## 3. Gate results (all green)

| Gate | Result |
|------|--------|
| `make check` | ✅ all modules + merged kernel verify |
| `make canvas-run` | ✅ VM asserts pass (result 0) |
| `make kernel` | ✅ 64-bit PVH ELF builds |
| `make verify` | ✅ ELF entry, `_start`, `curlee_main`, PVH note |
| `make qemu-smoke` | ✅ serial `Hello World from JOE!` (PVH/VGA fallback, no `FB:`) |
| `make iso` | ✅ text-mode ISO boots, serial `FB: 1` + `Hello World from JOE!` |
| `make iso-fb` | ✅ fb-mode ISO builds |
| `make qemu-fb-smoke` | ✅ **PASS** — serial log contains `FB: 1` |

### The acceptance proof

```
[A:0000000000109B18]            # %ebx captured (32-bit entry)
[T:0000000000000388]            # info total_size 0x388
... tags walked ...
[t:0000000000000008]            # framebuffer tag (type 8)
[s:0000000000000026]            # size 0x26
[a:00000000FD000000]            # fb_addr = VBE LFB
[p:0000000000000A00]            # pitch 2560
[w:0000000000000280]            # width 640
[h:00000000000001E0]            # height 480
[b:0000000000000020]            # bpp 32  -> accepted
FB: 1                           # fb_ready()==1 marker (gate grep)
Hello World from JOE!           # serial path still works
```

## 4. Notes

- The **text-mode ISO also prints `FB: 1`** now: the kernel's own framebuffer
  request tag (type 5) makes GRUB set up the LFB regardless of gfxterm. This is
  desirable (the renderer activates in both ISO modes) and does not affect the
  PVH path (`fb_ready()` stays 0 there → VGA fallback → `qemu-smoke`).
- `kernel/libgcc32.c` is linked only into `kernel-grub.elf`; the PVH path
  (native 64-bit) never references it.
- VirtualBox: the same 32-bit GRUB entry boots under VBox; the framebuffer
  request tag makes VBox's VBE provide an LFB too. Not CI-gated here.
- When the Curlee codegen import fix lands, `scripts/build-kernel.sh` is
  deleted (per the standing constraint); `libgcc32.c` and the 32-bit GRUB path
  remain.

## 5. Follow-up work items (open)

These are tracked here (markdown) as the source of truth for the JOE repo;
optionally mirrored as GitHub issues when the repo gets issue tracking.

### 5.1 Phase 2f — PVH/VBE framebuffer fallback (roadmap row 2f)

- **Problem:** the QEMU `-kernel` (PVH) path — the primary dev loop and
  `make qemu-smoke` — has **no multiboot2 info structure**, so `mb2_parse()`
  finds no framebuffer tag and `fb_ready()` stays 0. The renderer therefore
  never draws under `qemu -kernel`; only the VGA text fallback runs.
- **Goal:** activate the linear framebuffer on the PVH path too, so
  `make qemu-serial`/`make qemu-display` show the demo scene.
- **Candidate approaches** (pick one, issue-gate):
  1. **VBE/EDID probe in C**: a small driver that queries the VGA BIOS/VBE
     (int 0x10 via a real-mode thunk, or the Bochs/QEMU stdvga MMIO interface)
     to discover a linear framebuffer and set it up itself. Most portable;
     works for QEMU and VirtualBox.
  2. **QEMU `-kernel` with a framebuffer**: QEMU's `-kernel` can pass a
     framebuffer via the PVH spec's "e820 / boot params" extensions or by
     using `-device VGA` + a fixed known LFB base — but the address is not
     currently surfaced to the kernel on this path.
  3. **Add a PVH boot-params struct**: QEMU's PVH entry (XEN_ELFNOTE_PHYS32_ENTRY)
     only guarantees an entry point; a custom boot-params handshake would
     require QEMU support (out of scope for now).
- **Acceptance:** `make qemu-display` (or a new `make qemu-fb-pvh-smoke`) shows
  the demo scene under `qemu -kernel`, with `fb_ready()==1`.

### 5.2 Curlee codegen import crash (upstream, in `~/Projects/curlee`)

- `curlee build` (freestanding codegen) crashes on programs with `import`
  (`std::filesystem::filesystem_error`), which is why `kernel.curlee` still
  goes through `scripts/build-kernel.sh` single-TU merge. This is an
  **upstream issue-gated work item** in the Curlee repo, not JOE; when it
  lands, delete `build-kernel.sh` and import the modules natively.

### 5.3 VirtualBox framebuffer CI gate (optional)

- `scripts/vbox-setup.sh` boots the ISO in a real VirtualBox VM. The
  framebuffer request tag should make VBox's VBE deliver an LFB; a serial
  `FB: 1` assertion there would extend the acceptance proof beyond QEMU.
  Not CI-gated today (VBox requires the host virtualization extension and
  KVM unloading — see the script's `handle_kvm_conflict`).
