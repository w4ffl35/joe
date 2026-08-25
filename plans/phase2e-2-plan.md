# JOE OS — Phase 2e-2: 32-bit multiboot2 trampoline entry (plan)

Status: **PLANNED** (architect review)
Depends on: Phase 2e groundwork (`kernel/mb2.c`, `kernel/fb.c` blitter, `make iso-fb`, `make qemu-fb-smoke`)
Goal: Make `make qemu-fb-smoke` pass — the GRUB ISO path must deliver a **trusted** multiboot2
framebuffer tag to `mb2_parse()` so `fb_ready()==1` and the `FB:` serial marker proves the
renderer path end-to-end. Keep `make verify`, `make qemu-smoke`, `make iso` green.

---

## 1. Problem recap (from `docs/phase2e-architecture.md` §7)

GRUB's x86-64 ELF entry (ELFCLASS64, 64-bit `_start`) is the current GRUB-path entry. Empirically:

1. GRUB's 64-bit entry does **not** pass the multiboot2 info pointer in `%ebx` (reads 0).
2. GRUB's 64-bit ELF loader does **not** reliably load `.data` initializers (`fb_addr` reads 0).
3. `mb2_parse()` on garbage corrupts `fb_addr`; unguarded VBE writes to 0xFD000000 fault.
4. The only clean config found was a hardcoded VBE constant — but activating `render_frame`
   faults on an unmapped framebuffer.

**Root cause:** the multiboot2 spec only guarantees `%ebx` = info pointer for **32-bit protected-mode
entries**. GRUB enters ELFCLASS64 kernels in long mode with undefined `%ebx` and lazy `.data`.

**Fix (documented standard):** boot with a **32-bit entry** (where `%ebx` is guaranteed), capture the
info pointer, set up paging + long mode, and jump to the existing 64-bit `curlee_main`.

---

## 2. Design decision: ELFCLASS — kernel stays 64-bit, entry is 32-bit

The whole codegen + drivers are compiled **64-bit** (`-ffreestanding -m64`, `int64_t` ABI). Recompiling
the entire kernel as ELFCLASS32 (`-m32`) would change every ABI, pointer size, and the Curlee `Int`
mapping — high risk for zero benefit.

**Chosen:** keep all C/Curlee objects **ELFCLASS64** (same codegen as the PVH path), and make only the
boot stub a **32-bit entry**:

- `kernel/boot.S` is assembled **`--32`** → it is an ELFCLASS32 object containing:
  - the multiboot2 header (`.multiboot2`, `MB2_ARCH=0` i386),
  - the **32-bit entry** `_start` (`.code32`) — GRUB enters here in 32-bit protected mode with
    `%ebx` = trusted info pointer,
  - its **own GDT** (code/data 32-bit + code/data 64-bit selectors) and **identity page tables**
    (PML4 → PDPT → PD → 2 MiB pages covering the first 4 GiB) in a dedicated `.boot32` section,
  - the long-mode transition: set PAE, load CR3 = PML4, set EFER.LME, enable paging, load 64-bit
    GDT, far jump into a `.code64` trampoline label, then `jmp curlee_main`.
- The **final link** (`ld` with the existing 64-bit `linker.ld` + a `.boot32` output section) produces
  an **ELFCLASS64** kernel whose `e_entry = _start` (the 32-bit entry, absolute address).
- GRUB's `multiboot2` loader: reads the 32-bit header, loads the ELF (class-agnostic loader),
  enters the kernel at `e_entry` in **32-bit protected mode** with `%ebx` valid — exactly the
  documented multiboot2 handoff. The kernel then brings itself to long mode.

This is the Linux/OSDev standard pattern and requires **zero changes** to the PVH/QEMU path
(`crt0.S` + `kernel.elf` unchanged) and **zero changes** to Curlee codegen/runtime.

### Why not a 32-bit ELF for the whole kernel?

`curlee_main` (codegen) is `int64_t curlee_main(void)` with 64-bit ABI. Recompiling `kernel.c`
`-m32` would break the `Int`→`int64_t` mapping (`long long` is still 64-bit in `-m32`, but pointers
become 32-bit and `Phys<U16>*`/`fb_addr` semantics change), plus `rt.c`'s `size_t` and inline asm
assumptions. Not worth it — and the mixed-class link is the proven approach.

---

## 3. Architecture

```mermaid
flowchart LR
  A[GRUB multiboot2] -->|32-bit mode, ebx = info ptr| B[boot.S _start .code32]
  B -->|store ebx to mb2_info_addr .boot32| C[build identity page tables in .boot32]
  C -->|lgdt + PAE + CR3 + EFER.LME + paging| D[long mode]
  D -->|far jump to .code64 trampoline| E[curlee_main 64-bit]
  E --> F[main: fb_init]
  F --> G[fb.c: mb2_parse trusted info]
  G -->|fb_addr pitch width height| H[fb_ready = 1]
  H --> I[render_frame draws demo]
  I --> J[serial FB: marker]
  J --> K[qemu-fb-smoke PASS]
```

### 3.1 `kernel/boot.S` (rewritten, assembled `--32`)

```
.section .multiboot2, "a"        /* 32-bit header: magic, MB2_ARCH=0, size, checksum, end tag */
.section .boot32, "ax"           /* 32-bit protected-mode entry + GDT + page tables */
.code32
.global _start
_start:
  /* GRUB enters here: 32-bit PM, %ebx = trusted mb2 info ptr (spec-guaranteed) */
  mov %ebx, mb2_info_addr         /* absolute store into .boot32 data (works in 32-bit PM) */
  /* 1. memzero the page-table region (in .boot32, not .data — GRUB won't .data-init it) */
  /* 2. Build identity PML4/PDPT/PD covering 0..4 GiB with 2 MiB pages, PS=1, RW=1, P=1 */
  /* 3. lgdt (32-bit GDT with 64-bit code/data descriptors) */
  /* 4. mov %cr4, %eax; or $0x20 (PAE); mov %eax, %cr4 */
  /* 5. mov $pml4, %eax; mov %eax, %cr3 */
  /* 6. mov $0xC0000080, %ecx; rdmsr; or $0x100 (EFER.LME); wrmsr */
  /* 7. mov %cr0, %eax; or $0x80000001 (PG|PE); mov %eax, %cr0 */
  /* 8. ljmp $GDT64_CODE, $trampoline_64 */
.code64
trampoline_64:
  movabs $__stack_top, %rsp       /* reuse the existing linker-provided stack */
  jmp curlee_main                 /* no args: curlee_main(void) */
  /* curlee_main returns only on failure; halt loop after */

.section .boot32.data, "aw"       /* or .data within .boot32 region */
.balign 4096
gdt32:  ...                       /* null, 32-bit code, 32-bit data, 64-bit code, 64-bit data */
gdt32_desc: .word ..., .long ...  /* lgdt operand (absolute 32-bit addr) */
.balign 4096
pml4:   .quad 0 ... 512 entries    /* identity map; first PDPT entry at pml4[0] */
pdpt:   ...
pd:     ...                        /* 512 x 2 MiB pages = 1 GiB (or 4 GiB via 4 entries) */
mb2_info_addr: .quad 0            /* written by _start before long mode */
```

Key points:
- **`mb2_info_addr` lives in the `.boot32` region**, not `.data` — GRUB will not `.data`-init it
  (finding #2), but `_start` writes it from `%ebx` **before** any paging, so it's trusted by the
  time `curlee_main` runs.
- The GDT + page tables are **statically emitted** (full identity map), so no runtime construction
  loops are needed beyond an optional memzero. Static data in `.boot32` is safe: the kernel reads it
  from physical memory identity-mapped, exactly like the `.data` the current stub already relies on
  for `mb2_info_addr` (the only difference: the value is **written** by `_start`, not loaded).
- `curlee_main` is a **64-bit** symbol in the ELFCLASS64 object — the far jump uses its absolute
  address `< 4 GiB` (linker script keeps everything below 4 GiB, so no trampoline thunk needed).

### 3.2 New linker script variant `scripts/linker-grub.ld` (or inline in Makefile)

Mirror `runtime/linker.ld` but:
- `ENTRY(_start)` where `_start` is the `.boot32` 32-bit entry.
- Place `.boot32` (`.multiboot2` header + `.code32` + GDT + page tables) **first**, at `0x100000`,
  so the 32-bit entry is at a low, known address and the page tables land in usable low memory.
- Keep `.text/.rodata/.data/.bss/.stack` (64-bit) after it, same `__bss_start/__bss_end/__stack_top`.
- Keep `.note.Xen` for the PVH path — but note the GRUB path uses a separate ELF, so the PVH note
  stays in `kernel.elf` (crt0 path) and the GRUB-path ELF doesn't strictly need it (harmless to keep).

**Alternative:** keep `runtime/linker.ld` and add a `.boot32` output section that swallows the
`.boot32*` input sections before `.text`. This is lower-risk (same script, one new output section) and
avoids diverging from the Curlee runtime script. **Chosen:** extend `runtime/linker.ld` via a small
wrapper or direct edit? — see "Decisions to confirm" below.

### 3.3 Makefile — GRUB path only

Change the `kernel-grub.elf` recipe:
- `$(AS) --32 $(BOOT_ASM) -o $(BUILD_DIR)/boot.o` (was `--64`).
- All other objects (`kernel-grub.o`, `driver.o`, `vga_setup.o`, `fb.o`, `mb2.o`, `rt.o`) stay
  **64-bit** — same compile flags as today (they already are 64-bit).
- `$(LD) -nostdlib -static -T <linker with .boot32> ...` → ELFCLASS64, `e_entry = 32-bit _start`.
- `make iso` / `make iso-fb` unchanged (they consume `kernel-grub.elf`).

No change to the PVH path (`kernel.elf`, `crt0.S`, `qemu-smoke`) — it stays 64-bit end-to-end.

### 3.4 `kernel/mb2.c` — consume the trusted pointer

The parser already exists and is guarded (`mb2_info_addr != 0`, `< 1 MiB`, sane `total_size`, only
32bpp + nonzero `fb_addr`). With the 32-bit entry, `mb2_info_addr` now holds a **trusted** GRUB
pointer, so `mb2_parse()` will find the framebuffer tag and fill `fb_addr/pitch/width/height`.
**No code change required** — just confirm the symbol linkage (`extern unsigned long long mb2_info_addr`
already matches the `.boot32` `.quad`). The guards remain as defense-in-depth.

### 3.5 `kernel/fb.c` — `fb_init()` activates

Change:
```c
void fb_init(void)
{
    mb2_parse();            // trusted info from the 32-bit entry
}
```
Keep the VBE LFB constant (0xFD000000) as a **guarded fallback** — only used if the multiboot2 tag is
absent, and only when the address is plausible (the empirical fault was from writing a garbage/unmapped
address; `fb_ready()==0` must be the safe default). Bounds checks and `fb_ready()` stay as-is.

### 3.6 `kernel/kernel.curlee` — serial `FB:` marker

Add a serial marker so the gate can assert the framebuffer path ran:
```curlee
fn serial_fb_marker() -> Unit {
  curlee_putc(70);  // F
  curlee_putc(66);  // B
  curlee_putc(58);  // :
  curlee_putc(32);  // space
  curlee_putc(49);  // 1  (fb_ready()==1)
  curlee_putc(10);  // newline
  return;
}
```
Call it in `main` right after `fb_init()` when `fb_ready()==1`. Keep the existing
`Hello World from JOE` + VGA fallback for `qemu-smoke` (PVH path — `fb_ready()` stays 0 there).

### 3.7 `make qemu-fb-smoke` (already exists)

Boots `build/joeos-fb.iso` (grub.cfg `fb` variant: `gfxmode=640x480x32`, `gfxpayload=keep`,
`terminal_output gfxterm`) and greps `FB:` in the serial log. **No Makefile change needed** for the
gate itself; it goes green once the trampoline + `fb_init` activation land.

---

## 4. Files changed

| File | Change |
|------|--------|
| `kernel/boot.S` | Rewrite: `.code32` entry + GDT + identity page tables + long-mode transition + `.code64` trampoline; assembled `--32` |
| `runtime/linker.ld` OR new `scripts/linker-grub.ld` | Add `.boot32` output section (header + code + GDT + PTs + `mb2_info_addr`); keep `ENTRY(_start)`, `__bss_*`, `__stack_top` |
| `Makefile` | `kernel-grub.elf`: `$(AS) --32 boot.S`; link with the `.boot32`-aware script; rest stays 64-bit |
| `kernel/fb.c` | `fb_init()` calls `mb2_parse()`; keep guarded VBE fallback |
| `kernel/mb2.c` | (confirm only — parser already consumes `mb2_info_addr`) |
| `kernel/kernel.curlee` | Add `serial_fb_marker()` + call when `fb_ready()==1` (merged via `build-kernel.sh`, no import change) |
| `docs/phase2e-architecture.md` | Update §7/§8 status (blocker resolved) |
| `docs/phase2e-2-report.md` | NEW: implementation + empirical results |
| `docs/phase2-report.md` | Update Phase 2e verdict |

---

## 5. Verification plan (gates)

| Gate | Expectation |
|------|-------------|
| `make check` | unchanged — all modules + merged kernel still verify |
| `make canvas-run` | unchanged — pure math asserts pass |
| `make kernel` | unchanged — PVH/QEMU ELF builds (64-bit crt0) |
| `make verify` | unchanged — ELF entry, `_start`, `curlee_main`, PVH note |
| `make qemu-smoke` | unchanged — serial `Hello World from JOE` (PVH/VGA fallback; `fb_ready()`=0) |
| `make iso` | GRUB text-mode ISO boots (trampoline also works in text mode; VGA fallback still hit) |
| `make iso-fb` | builds the fb ISO |
| `make qemu-fb-smoke` | **FLIPS GREEN**: serial log contains `FB:` → `fb_ready()==1` |

Dev loop order: `make check` → `make canvas-run` → `make kernel` → `make verify` →
`make qemu-smoke` → `make iso-fb` → `make qemu-fb-smoke`.

---

## 6. Risks / mitigations

| Risk | Mitigation |
|------|------------|
| GRUB loads the ELF at a different base than `0x100000` (page tables' absolute addresses wrong) | Link at `0x100000` (multiboot2 standard); identity map covers the whole low 4 GiB so any load base works for the entry; page tables/GDT use absolute physical addresses computed at link time |
| Page tables > 4 GiB (64-bit `pml4` addresses) | Link everything below 4 GiB (existing layout does); verify with `readelf -l` |
| `curlee_main` entry is a 64-bit absolute address > 32-bit reachable | Linker keeps `.text` below 4 GiB; far jump to the absolute address is fine in long mode |
| `mb2_info_addr` `.quad` in a 32-bit-object section vs 64-bit C `extern` | It's just 8 bytes of storage; the C side reads it as `unsigned long long` — ABI matches (verified by symbol type/size) |
| GDT/page-table memzero | Static tables emitted with explicit `.quad 0` padding; no runtime zeroing needed. If a runtime clear is desired, memzero the `.boot32.data` region before use |
| GRUB's `multiboot2` loader vs `multiboot` (v1) | Header magic `0xE85250D6` + `MB2_ARCH=0`; already used; unchanged |
| QEMU `-kernel` uses the PVH path (no 32-bit entry) | `kernel.elf` (crt0) is untouched; only the GRUB-path `kernel-grub.elf` gets the trampoline |
| VirtualBox text-mode ISO | Same trampoline boots text mode; VGA fallback keeps `qemu-smoke`-style behavior; `fb_ready()` may be 0 (no gfxterm) → VGA text path (unchanged) |

---

## 7. Decisions to confirm

1. **Where the `.boot32` section lives:** (a) extend `runtime/linker.ld` (Curlee runtime — but rule
   says don't modify `~/Projects/curlee` unless asked), or (b) **new `scripts/linker-grub.ld`** in
   joeos that includes the runtime script's layout plus `.boot32` (preferred — stays inside the
   workspace). Confirm (b).
2. **GDT/page tables statically emitted vs memzero + build at runtime:** static is simpler and
   deterministic (fits "deterministic verification" law); confirm static.
3. **`FB:` marker text:** `FB: 1\n` (gate greps `FB:`). Confirm the exact marker.
4. **VBE fallback in `fb_init`:** keep the constant as a guarded fallback, or drop it entirely and
   rely solely on the multiboot2 tag? Recommend keep-guarded (harmless when unmapped — `fb_ready`
   stays 0 unless the tag validated).
