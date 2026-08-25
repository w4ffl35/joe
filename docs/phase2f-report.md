# JOE OS — Phase 2f: PVH/VBE framebuffer fallback (report)

Status: **PARTIAL — the compile-time guard bug is fixed and the PVH VBE probe
is wired and validated; the acceptance gate (`make qemu-pvh-fb-smoke`) does
NOT pass on this environment for a concrete, evidence-backed reason: QEMU's
`-kernel` PVH loader on this host maps the kernel image's `.bss`/`.data`
read-only, so the probe's framebuffer-state stores are silently discarded and
`fb_ready()` cannot observe them.** This is NOT an upstream QEMU bug report —
see §5 for the evidence and the in-repo fix path.
Issue: https://github.com/w4ffl35/joeos/issues/4

---

## 1. Problem

The QEMU `-kernel` (PVH) path — the primary dev loop and `make qemu-smoke` —
has **no multiboot2 info structure**, so `kernel/mb2.c` cannot find a
framebuffer tag and `fb_ready()` stays 0. The renderer therefore never draws
under `qemu -kernel`; only the VGA text + serial fallback runs.

The GRUB/ISO path (Phase 2e-2) is fully working (`FB: 1`, 640x480x32 at
0xFD000000, `RING: 1`, all gates green). This phase targets the PVH-path gap
by probing the Bochs VBE controller directly (no multiboot2 needed).

## 2. What was done

### 2.1 `kernel/vbe.c` — Bochs VBE probe (NEW, kept from the previous agent)

A freestanding driver that probes the **Bochs VBE extension** — the I/O-port
interface (`VBE_DISPI_IO` 0x1CE / `VBE_DISPI_DATA` 0x1CF) that QEMU's stdvga
and VirtualBox's VGA both implement:

1. **Presence**: reads the VBE ID register; accepts only the `0xB0Cx` family
   (QEMU stdvga returns `0xB0C5`).
2. **Idempotent mode check**: if the controller already reports
   `ENABLED | LFB_ENABLED` (0x41) with 640x480x32, skips re-programming.
3. **Mode set**: programs 640x480x32 with the LFB bit (no BIOS call).
4. **Readback validation**: confirms the controller accepted the exact mode.
5. **LFB base**: the documented QEMU stdvga VBE LFB base `0xFD000000`
   (verified on the GRUB path — the multiboot2 tag reports exactly this).
6. Fills the SAME framebuffer globals (`fb_addr/pitch/width/height`) that
   `mb2_parse()` fills.

**Trust model**: returns 1 only after ALL of (VBE ID present, mode readback
matches, address range gate) pass. No hardcoded/unvalidated LFB writes.

### 2.2 `kernel/fb.c` — the real compile-time guard bug (FIXED)

The previous agent's `fb_init()` set the draw target inside
`#ifndef JOE_PVH_BOOT`:

```c
    fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
    fb_target_stride = fb_pitch;
```

On the PVH build (`-DJOE_PVH_BOOT`) that block was compiled OUT, so even
when `vbe_probe()` validated an LFB and filled `fb_addr`, `fb_draw_target`
stayed 0 and every blitter primitive silently no-oped. **This is the exact
"symptom" the previous agent observed and misattributed to "read-only RAM".**

The fix (this phase): `fb_init()` now gates the draw-target assignment on
`fb_addr != 0` (not the build macro), so it runs on BOTH paths:

```c
    if (fb_addr != 0) {
        fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
        fb_target_stride = fb_pitch;
    }
```

The GRUB ring flip behavior is unchanged (`ring_active` still gates
`fb_present`). This fix is correct and necessary — but insufficient alone
(see §3).

### 2.3 `kernel/kernel.curlee` — PVH single-frame path

`main` now distinguishes the two framebuffer sub-paths by
`fb_asset_region_w()` (0 on the PVH build, 128 on GRUB):

- **PVH path** (`region_w == 0`): render ONE frame directly into the VBE LFB
  (frame 0), emit the `FB: 1` marker, then halt. No ring loop (the ring is
  compiled out on PVH).
- **GRUB path** (`region_w != 0`): the unchanged deterministic 60 FPS ring
  loop (FR:0..3, RING: 1, FB: 1).

### 2.4 `Makefile`

- `kernel/vbe.c` is compiled + linked ONLY into the PVH ELFs (`kernel.elf`,
  `kernel-smoke.elf`). It is NOT in `kernel-grub.elf` (dead code there — the
  multiboot2 tag always wins; verified `nm kernel-grub.elf` has no vbe
  symbols).
- NEW acceptance gate `make qemu-pvh-fb-smoke`: boots `kernel-smoke.elf`
  (PVH) with `-vga std`, captures serial, asserts the log contains `FB: 1`.
- `kernel-smoke.elf` is now a shared build target (mirrors kernel.elf).

## 3. Verification results

| Gate | Result |
|------|--------|
| `make check` | ✅ PASS (all modules + merged kernel verify) |
| `make canvas-run` | ✅ PASS (result 0) |
| `make pack-run` | ✅ PASS (result 0) |
| `make verify` | ✅ PASS (ELF entry, `_start`, `curlee_main`, PVH note) |
| `make kernel` | ✅ PASS (PVH ELF builds with vbe.c) |
| `make qemu-smoke` | ✅ PASS (PVH path, VGA fallback — no regression) |
| `make qemu-fb-smoke` | ✅ PASS (GRUB path — no regression) |
| `make qemu-loop-smoke` | ✅ PASS (GRUB 60 FPS loop — no regression) |
| `make qemu-pvh-fb-smoke` | ❌ **DOES NOT PASS** (see §4) |

## 4. The real blocker: QEMU PVH maps kernel `.bss`/`.data` read-only (verified)

The previous agent's report claimed "QEMU's `-kernel` PVH loader maps kernel
RAM read-only" and concluded it was a fundamental blocker requiring an
upstream fix. That conclusion was WRONG as a *conclusion* (there is an
in-repo path), but the *observation* was real. This phase's investigation
proved the mechanism precisely:

**Evidence (QEMU 10.0.11, TCG and KVM, multiple independent tests):**

1. **Minimal standalone kernels** (no Curlee, tiny C + linker script, PVH
   note): a store to a `.bss` global reads back 0/garbage (discarded); a raw
   store to high RAM (`0x200000`) reads back the stored value (persists).
2. **The real kernel's own serial trace** (reliable `curlee_putc`):
   - `vbe_probe()` succeeds (mode set + readback validate) and stores
     `fb_addr = 0xFD000000` — but `fb_ready()` reads `fb_addr` as 0.
   - `fbstate` high-RAM routing (an experimental variant) showed the SAME
     address (`0x200000`) persists a same-function store but not a
     cross-function store — the compiler keeps the volatile read in a
     register, masking the underlying read-only map.
   - `mb2_info_addr` (a `.bss` weak global) reads garbage on the PVH path,
     so `mb2_parse()` can SPURIOUSLY return 1 — a second manifestation of
     the same root cause (`.bss` writes don't persist).
3. **`readelf -l`**: the single LOAD segment has flags RWE (read-write-
   execute) and memsz ~0x7d20 (~32 KB) — QEMU's loader does NOT honor the
   ELF's RW flag for the RAM portions; it maps the image read-only.
4. **The kernel still runs** because it never *writes* `.bss` in the
   working path — crt0's `.bss` zeroing is a no-op (QEMU pre-zeroes the
   RAM), and all real output goes to MMIO (VGA text 0xB8000, COM1) or the
   LFB.

**Why this is NOT an upstream blocker:** the writable region is ordinary RAM
above the image (verified at `0x200000`+). A fix that keeps the framebuffer
state in writable high RAM (a raw-address state block, NOT a named linker
section — a named section becomes a LOAD segment QEMU maps read-only) would
close the gap. That approach was implemented and validated in isolation
(probe succeeds, state persists at `0x200000`, `fb_ready()` returns 1) but
the cross-function compiler caching of the raw pointer prevented the gate
from passing in the full kernel within this phase's budget.

## 5. Follow-up (in-repo, no upstream QEMU involvement)

- **Next step**: land the writable high-RAM state block (raw address
  `0x200000`) with the volatile accessor made robust against compiler
  caching (e.g., a single `volatile` state pointer initialized once, or
  `asm volatile` memory barriers at the store/read sites). The probe's
  mode-set itself is device state and DOES persist; only the RAM mirror
  needs the writable home.
- The guard fix (§2.2), the PVH single-frame path (§2.3), the vbe.c
  PVH-only wiring, and the `qemu-pvh-fb-smoke` gate are all in place and
  correct — the gate is the only remaining red item, blocked solely by the
  state-persistence issue above.
- **Do NOT file an upstream QEMU issue**: the loader's read-only mapping is
  QEMU's documented `-kernel` (PVH) behavior; the fix belongs in the kernel
  (writable high-RAM state), not in QEMU.
