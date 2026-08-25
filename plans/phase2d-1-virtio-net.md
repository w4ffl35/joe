# Phase 2d-1: VirtIO-net driver (RX/TX ring, init, link-up)

Parent: [#3 Phase 2d: LLM bridge (VirtIO-net / TCP + JSON)](https://github.com/w4ffl35/joeos/issues/3)
Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) (single source of truth)
Status: OPEN — spec (filed as GitHub issue #5)

## Summary

Implement the **guest NIC driver** that gives the kernel a network transport
under QEMU: a VirtIO-net device driven over PCI, with RX/TX virtqueue rings,
initialization, and link-up detection. This is the bulk of the Phase 2d work
and the prerequisite for every later sub-issue (2d-2 TCP, 2d-3 JSON, 2d-4
harness). Everything is freestanding C — no libc, no malloc, static buffers.

## Current state (what exists)

- No PCI enumeration, no port I/O helpers, no network driver anywhere in the
  kernel. The only device interaction today is the multiboot2 framebuffer tag
  (`kernel/mb2.c`) and direct VGA/COM1 MMIO writes (`kernel/vga_setup.c`,
  `kernel/putc_driver.c`).
- Build: two paths — 64-bit PVH `kernel.elf` (`qemu -kernel`, crt0.S) and
  32-bit GRUB `kernel-grub.elf` (ISO, boot.S). **PVH budget constraint**
  (verified empirically in `kernel/fb.c`): QEMU's PVH loader silently refuses
  ELFs whose LOAD segment (file + BSS) exceeds a hard budget (~0x8000-0x10000
  bytes of memsz beyond the base), so large static buffers must be compiled
  OUT on the PVH path (`JOE_PVH_BOOT` macro) and compiled IN on the GRUB
  path. The NIC rings must follow the same pattern (sizing LOCKED below).
- The tool-call queue (`fb_tool_enqueue` etc. in `kernel/fb.c`,
  `assets.curlee`) is the consumer the LLM bridge feeds — not needed by this
  sub-issue, but the RX path should expose a byte-buffer API the TCP layer
  (2d-2) will consume.

## Goal

A deterministic, probe-observable VirtIO-net bring-up under QEMU:

1. Minimal **PCI config-space probe** (ports `0xCF8`/`0xCFC`) that finds the
   virtio-net device and reads its BAR. Probe table (virtio spec — correct
   transitional/legacy IDs):
   - `0x1AF4:0x1041` — **modern transitional net** (default QEMU
     `virtio-net-pci`)
   - `0x1AF4:0x1000` — **legacy net** (QEMU with `disable-modern=on` forces
     this ID)
   - `0x1AF4:0x1FE9` — **modern-only net** (VERSION_1-only device; not used
     by our smoke path, but the probe should recognize it so a future
     modern path can adopt it)
   Note: `0x1001` is the **block** device — NOT net — and must never be
   matched by this driver.
2. **Device initialization** for a *legacy (virtio 0.9.5)* virtio-net
   interface via the I/O BAR register set. Legacy mode is NOT selected by
   "clearing a device feature": it means the **driver** offers features
   without bit 32 (`VIRTIO_F_VERSION_1`) and uses the legacy-only register
   layout (QueuePFN / GuestPageSize / legacy status in the I/O BAR). On the
   QEMU side, legacy is forced with `-device virtio-net-pci,disable-modern=on`
   (or `-global virtio-net-pci.disable-modern=on`). The legacy path never
   offers VERSION_1 — that is what makes the device behave legacy.
3. **RX/TX virtqueues** (legacy split layout: desc/avail/used rings) over
   static buffers. Sizing LOCKED (see "PVH buffer sizing (LOCKED)" below):
   **2 RX buffers × 2048 B + 2 TX buffers × 2048 B** by default.
4. **Link-up detection**: device status / link feature read; serial probe
   markers (`NET: 1` = PCI found, `NET: 2` = device ready, `NET: 3` = link up).
5. A **receive one frame** smoke path — deterministic frame source = the
   GUEST ARPs the gateway (see "Frame-injection mechanism" below).

## Probe order & transport (LOCKED)

- Probe all PCI bus/device/function slots for vendor `0x1AF4`; match device
  IDs `0x1041` (modern transitional) and `0x1000` (legacy). `0x1FE9`
  (modern-only) is recognized but NOT initialized on the smoke path.
- **Preferred transport: legacy (virtio 0.9.5) via the I/O BAR.** The smoke
  path boots with `disable-modern=on` so the device presents legacy ID
  `0x1000` + legacy registers — deterministic, small, no capability walk, no
  MSI-X. If a device only offers modern (`0x1041` with no legacy BAR), fall
  back to modern MMIO via the capability layout (documented; not required
  for the smoke path).
- The driver NEVER offers `VIRTIO_F_VERSION_1` (bit 32) on the legacy path.

## Frame-injection mechanism (LOCKED)

The smoke path does NOT rely on host-side ping/ARP injection. QEMU's slirp
user-net **automatically answers ARP for the gateway alias `10.0.2.2`**, so
the deterministic frame source is simply the **guest ARPing the gateway**
(an ARP request for `10.0.2.2` from the guest gets an ARP reply from slirp,
delivering the first RX frame). This is exactly the ARP step 2d-2 needs, so
2d-1's smoke and 2d-2's ARP share the same mechanism. The driver reports the
received frame via serial (`RX: <len>`), then 2d-2 takes over.

## PVH buffer sizing (LOCKED)

- **Default (option 2):** the smallest workable rings — **2 RX buffers ×
  2048 B + 2 TX buffers × 2048 B** — plus the ring structs (desc/avail/used
  arrays). This fits the PVH LOAD budget, so the NIC stays active on the
  `-kernel` dev loop (`make qemu-serial` can exercise it).
- **Fallback trigger (option 1):** if the linked PVH LOAD segment (file +
  BSS memsz) exceeds the ~0x8000-0x10000-byte budget observed in
  `kernel/fb.c` (check with `size build/kernel.elf` / `readelf -l`), flip to
  compiling the NIC + rings OUT under `JOE_PVH_BOOT` (like `fb.c`'s
  region/ring) and gate `qemu-net-smoke` on the GRUB/ISO path
  (`kernel-grub.elf`, which has no budget limit). Implementer must NOT
  re-derive this — apply option 2 first, flip only on a measured budget
  breach, and document which mode is active in the `kernel/virtio_net.c`
  header (mirror the `fb.c` header style).
- All buffers static; no malloc anywhere; ring size macros live beside the
  `JOE_PVH_BOOT` guard so a future size change is one line.

## Design decisions

- **Port I/O in freestanding C**: `outl/inl` via `__asm__ volatile` inline
  asm (no libc `sys/io.h`). Keep them in one small section of the driver
  file with a comment; mirror the `Phys<T>` discipline (raw access is
  trusted, gated by the driver being the only toucher).
- **Acknowledge buffers**: never reuse an RX buffer the driver is still
  parsing — the TCP layer (2d-2) owns RX buffers after handoff; the driver
  reclaims only buffers the stack has released. Fixed-slot discipline, no
  malloc.
- **No-NIC-safe probe**: the PCI probe MUST be a safe no-op when no
  virtio-net device is present — find nothing, zero state, no hang, no
  crash — because the existing smoke gates (`qemu-smoke`, `qemu-fb-smoke`,
  `qemu-loop-smoke`) boot WITHOUT `-netdev`/`-device` args (see acceptance
  criterion 6).

## Files

| File | Change |
|------|--------|
| `kernel/virtio_net.c` | NEW: PCI probe + legacy virtio-net init, RX/TX virtqueues, link status, frame RX/TX entry points; `JOE_PVH_BOOT` sizing guard |
| `kernel/net.h` | NEW: shared net constants + RX/TX buffer API (consumed by 2d-2) |
| `Makefile` | Compile `virtio_net.o` into both `kernel.elf` and `kernel-grub.elf` (with `JOE_PVH_BOOT` conditional buffers); new `qemu-net-smoke` target |
| `scripts/build_iso.sh` / QEMU invocations | Add `-netdev user` + `-device virtio-net-pci,disable-modern=on,netdev=n0` to the smoke/run targets that need the NIC |
| `kernel/kernel.curlee` | Externs + serial probe markers (`NET: 1..3`, `RX: <len>`) driven from `main` |

## Extern surface (Curlee window into the NIC)

```curlee
extern fn net_probe() -> Int;      // 1 = virtio-net PCI device found
extern fn net_init() -> Int;       // 1 = device ready (rings set up)
extern fn net_link_up() -> Int;    // 1 = link up
extern fn net_rx_len() -> Int;     // bytes in the current RX frame (0 = none)
extern fn net_rx_done() -> Unit;   // release the current RX buffer
extern fn net_tx_send(len: Int) -> Int;  // 1 = queued, 0 = no buffer
```

## Acceptance criteria

1. `make kernel` + `make verify` still pass (both ELF paths link).
2. New `make qemu-net-smoke`: boots with `-netdev user` +
   `-device virtio-net-pci,disable-modern=on,netdev=n0`, serial log contains
   `NET: 1`, `NET: 2`, `NET: 3` (PCI found → device ready → link up).
3. A received frame is reported (`RX: <len>` marker) — the guest's own ARP
   request for the gateway `10.0.2.2` draws slirp's automatic ARP reply,
   which is the deterministic first RX frame (no host-side injection, no
   live network).
4. No malloc/libc anywhere; all buffers static; PVH sizing follows the
   LOCKED decision above (option 2 default, option 1 fallback on a measured
   budget breach) and the active mode is documented in the file header.
5. All existing gates (`check`, `canvas-run`, `qemu-smoke`, `qemu-fb-smoke`,
   `qemu-loop-smoke`) remain green.
6. **No-NIC-safe (explicit):** boots with NO virtio-net device present — the
   PCI probe is a safe no-op (finds nothing, no hang, no crash) and all
   existing smoke gates stay green with the driver compiled IN
   (`kernel.elf` / `kernel-grub.elf` both link and boot with the NIC
   absent).

## References

- Issue #3 body (QEMU line: `-device virtio-net-pci,netdev=n0 -netdev user,id=n0`)
- Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) (markers `NET: 1..3`, extern names)
- `docs/phase2-architecture.md` §7 (roadmap row 2d), `docs/phase2e-2-report.md`
- Virtio spec: legacy virtio-net (0.9.5) I/O BAR register layout + split
  virtqueue (desc/avail/used); transitional/legacy PCI IDs `0x1041`/`0x1000`,
  modern-only `0x1FE9`; `disable-modern=on` for legacy — driver-side only,
  no device feature work
- `kernel/fb.c` header: the PVH LOAD-budget finding that motivates
  `JOE_PVH_BOOT`
