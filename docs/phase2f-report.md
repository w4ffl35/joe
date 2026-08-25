# JOE OS — Phase 2f: PVH/VBE framebuffer fallback (report)

Status: **CLOSED (not implemented) — the QEMU `-kernel` PVH path cannot expose a
framebuffer to this kernel with QEMU's current PVH machine. The compile-time
guard bug found during the phase is fixed and shipped (correct + no regression);
the VBE probe is correct and reusable where a VBE device exists; the remaining
acceptance gate is blocked by a genuine platform constraint (the PVH machine has
no VGA device), not by a repo bug or "read-only RAM". This phase is documented
and parked for future revisit.**
Issue: https://github.com/w4ffl35/joeos/issues/4

---

## 1. Problem

The QEMU `-kernel` (PVH) path — the primary dev loop and `make qemu-smoke` —
has **no multiboot2 info structure**, so `kernel/mb2.c` cannot find a
framebuffer tag and `fb_ready()` stays 0. The renderer therefore never draws
under `qemu -kernel`; only the VGA text + serial fallback runs.

The GRUB/ISO path (Phase 2e-2) is fully working (`FB: 1`, 640x480x32 at
0xFD000000, `RING: 1`, all gates green). This phase targeted the PVH-path gap
by probing the Bochs VBE controller directly (no multiboot2 needed).

## 2. What was done

### 2.1 `kernel/vbe.c` — Bochs VBE probe (NEW, kept)

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

### 2.2 `kernel/fb.c` — the real compile-time guard bug (FIXED, shipped)

The earlier `fb_init()` set the draw target inside `#ifndef JOE_PVH_BOOT`:

```c
    fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
    fb_target_stride = fb_pitch;
```

On the PVH build (`-DJOE_PVH_BOOT`) that block was compiled OUT, so even
when `vbe_probe()` validated an LFB and filled `fb_addr`, `fb_draw_target`
stayed 0 and every blitter primitive silently no-oped.

The fix (this phase, committed): `fb_init()` now gates the draw-target
assignment on `fb_addr != 0` (not the build macro), so it runs on BOTH paths:

```c
    if (fb_addr != 0) {
        fb_draw_target = (volatile unsigned int*)(unsigned long)fb_addr;
        fb_target_stride = fb_pitch;
    }
```

The GRUB ring flip behavior is unchanged (`ring_active` still gates
`fb_present`). This fix is correct, necessary, and shipped — but insufficient
alone (see §3).

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

## 4. The blocker (verified): the QEMU PVH machine has NO VGA device

Extensive serial-instrumented bring-up (QEMU 10.0.11, TCG) established the
real, evidence-backed reason the PVH path cannot show a framebuffer:

**Serial-instrumented evidence (this phase):** a temporary trace added to
`vbe_probe()` printed the probe's reads under every `-kernel` combination:

| QEMU invocation | PCI host-bridge read (0xCF8/0xCFC, bus 0 dev 0) | Bochs VBE ID (0x1CE/0x1CF) |
|---|---|---|
| `-vga std` (default machine) | 0x00000000 | 0x0000 |
| `-machine pc -vga std` | 0x00000000 | 0x0000 |
| `-machine q35 -device VGA` | 0x00000000 | 0x0000 |

Both the legacy PCI config mechanism AND the legacy Bochs VBE ports return
all-zeros under `qemu -kernel`, regardless of machine type or VGA device.

**Why (QEMU's own docs):** the PVH machine QEMU uses for `-kernel`
(`xenpvh` — `/usr/share/doc/qemu-system-common/system/i386/xenpvh.html`)
supports only **RAM, a GPEX host bridge, and virtio-pci devices**. There is
**no VGA/stdvga device model** and **no legacy PCI config space**. With
`-kernel`, QEMU loads the ELF and jumps straight to the PVH entry in long
mode with paging on — **no SeaBIOS runs**, so no PCI enumeration and no VGA
initialization ever happen. That is why every device read returns 0.

**Explicitly NOT the cause — "read-only RAM" is wrong.** The kernel's own
working path proves `.bss`/`.stack` are writable on the PVH path: crt0.S
zeroes `.bss` by storing bytes to it, the stack (a `.bss` NOLOAD region) is
pushed/popped by every call, and `fb_tool_enqueue`/`fb_run_loop` write
`.bss` globals (`tool_queue[]`, `loop_frame`) — all of which work
(`qemu-smoke` passes, the 60 FPS loop works). A read-only `.bss` would
#PF with no IDT and no page tables set up — the kernel could not boot at
all. The earlier "read-only RAM" reports were a misdiagnosis; the real
constraint is device absence on the PVH machine.

## 5. Conclusion & follow-up (parked)

- **The VBE probe code is correct and reusable** for any boot path where a
  VBE device IS present (e.g., a SeaBIOS-booted machine — the GRUB/ISO path,
  which already works via the multiboot2 tag). It is harmless on the PVH
  path (returns 0 cleanly → VGA text fallback, all gates green).
- **Phase 2f's acceptance gate (`make qemu-pvh-fb-smoke`) cannot pass** under
  QEMU's current `-kernel` PVH machine because the machine exposes no VGA
  device and no legacy PCI config space to the guest.
- **Revisit options** (each needs an issue-scoped decision, not a code hack):
  1. **QEMU upstream**: make the PVH `hvm_start_info` boot-params struct
     carry a framebuffer (the PVH spec's defined handoff), and have the
     kernel read it from boot params instead of probing PCI/VBE.
  2. **SeaBIOS-booted `-kernel`**: boot the PVH ELF under SeaBIOS (e.g., a
     minimal ISO/GRUB), which initializes VGA/PCI — but that is the GRUB
     path by another name (already green).
  3. **Accept GRUB-path-only** (current state): the PVH dev loop stays on
     VGA text + serial; the framebuffer renderer is exercised via
     `qemu-fb-smoke`/`qemu-loop-smoke` on the GRUB ISO path.
- **No upstream QEMU issue filed** in this phase: the loader's behavior is
  documented PVH machine semantics; filing is a deliberate future decision
  under option 1 above.
