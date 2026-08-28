# Phase 2g: virtio-blk storage driver (raw sector read, no filesystem)

Parent: [GitHub issue #26](https://github.com/w4ffl35/joeos/issues/26)
Status: DONE — the driver, the build-side disk-image mechanism, and the
`qemu-blk-smoke` gate are merged.

## Summary

A paravirtualized **virtio-blk** device driver that gives the kernel raw
sector reads under QEMU (`-device virtio-blk-pci`), following the exact
pattern proven twice with `virtio_net.c` → `kernel/virtio_net.curlee`
(gh issue #14, #296): legacy virtio 0.9.5 over the PCI I/O BAR, a split
virtqueue, per-build static buffer sizing, and a deterministic serial-marker
smoke gate. Written **directly in Curlee** — the language gaps that blocked
the net driver's original C port (address-of, runtime Phys read/write,
module-level state) are all closed, so there is no C-then-port step and no C
shim (kernel/ holds zero C files).

Scope (per the issue): read N sectors at a given LBA into a fixed buffer. No
write support, no filesystem — the raw contract is enough to load a single
embedded model blob from a disk image (the Native Inference milestone's
need), and a filesystem is a separate later concern.

## Transport & device (LOCKED — mirrors 2d-1)

- **Legacy virtio 0.9.5 via the PCI I/O BAR.** The smoke path boots QEMU
  with `-device virtio-blk-pci,disable-modern=on,drive=blk0`, which presents
  the legacy device ID `0x1001` and the legacy-only register layout
  (QueuePFN / legacy status / device config at BAR0+0x14). No capability
  walk, no MSI-X, no modern registers.
- **Probe IDs**: `0x1AF4:0x1001` (legacy block, the smoke path), `0x1042`
  (modern transitional block — recognized, driven only when it presents a
  legacy I/O BAR). `0x1FE8` (modern-only) is never initialized; the net IDs
  (`0x1000`/`0x1041`/`0x1FE9`) are never matched.
- **One request queue** (virtio-net's RX/TX pair collapses to a single
  queue): 3-page legacy layout — desc page 0, avail page 1, used page 2,
  4096-aligned base, `QueuePFN = phys >> 12`.

## Request shape (read path only)

A read is a 3-descriptor NEXT chain:

| desc | contents | flags |
|------|----------|-------|
| 0 | 16-byte request header: `type u32 = 0` (VIRTIO_BLK_T_IN), `reserved u32 = 0`, `sector u64 = LBA` | NEXT (1), next = 1 |
| 1 | data buffer (sectors × 512 bytes) | WRITE \| NEXT (3), next = 2 |
| 2 | 1-byte status (0 = VIRTIO_BLK_S_OK) | WRITE (2) |

The used ring reports the head id 0 with `len = sectors*512 + 1`.

**Verified live**: the request header is **16 bytes** (QEMU's
`VirtIOBlockOutHdr` is not packed; the sector u64 ends at offset 16). A
12-byte header draws `virtio-blk request outhdr too short` from QEMU — the
first smoke run hit exactly this and the header was fixed.

## Driver surface (genuine Curlee, codegen'd as curlee_blk_*)

```
fn blk_probe(pm) -> Int          // 1 = legacy-capable virtio-blk PCI device found
fn blk_init(pm) -> Int           // 1 = device ready (queue set up, DRIVER_OK)
fn blk_read(pm, lba, sectors) -> Int   // 1 = sectors read into the fixed buffer
fn blk_sector_byte(pm, i) -> Int       // byte i of the last read (0 if none/OOR)
fn blk_err() -> Int              // diagnostic: why the last blk_read() failed
```

- `blk_read` is **single-in-flight**: it builds the chain, publishes the
  avail ring, notifies, polls the used ring (bounded, ISR-yield — the
  net_rx_wait pattern), collects the completion (id == 0, len ==
  sectors*512+1, status == 0), and only then returns — so the caller owns
  the buffer until the next call.
- The ring index math uses the **runtime QUEUE_NUM** (not a hardcoded 256)
  with explicit `n >= 3 && head >= 0` guards — the probe-verified symbolic
  modulo shape.
- No-NIC-safe mirror (acceptance criterion 6): with no virtio-blk device
  `blk_probe()` finds nothing and every other function returns 0 — no hang,
  no crash; all existing gates boot WITHOUT `-drive`/`-device` and stay
  green.

## PVH sizing (LOCKED — option 1/option 2, gh issue #296 pattern)

| build | BLK_QMEM_PAGES | BLK_BUF_SECTORS | behavior |
|-------|----------------|-----------------|----------|
| GRUB (ISO — where the device runs) | 5 | 8 (4096 B) | full driver |
| PVH (`qemu -kernel`) | 1 | 1 | stubs: every base getter returns 0, blk_init() bails |

The PVH `-kernel` machine exposes **no legacy PCI config space**
(docs/phase2f-report.md §4), so the disk is only reachable where SeaBIOS
runs — the GRUB/ISO path, exactly like the NIC. The `qemu-blk-smoke` gate
boots a dedicated ISO (`joeos-blk.iso`) with the disk attached.

## Build-side disk image (acceptance criterion 4)

`scripts/make-blk-disk.sh <out.img> [blob_file] [lba]`:
- default: a 1 MiB zero-filled raw image with the **"JOE-BLK!" test pattern**
  (74 79 69 45 66 76 75 33) repeated over LBA 64..71 — the deterministic
  pattern `blk_pattern_byte` in kernel.curlee verifies;
- with a blob file: places the model blob at the given LBA (the Native
  Inference use case). No filesystem — a plain raw image.

## Smoke gate (acceptance criterion 3)

`make qemu-blk-smoke`: boots `joeos-blk.iso` with
`-drive file=build/blk-disk.img,if=none,id=blk0,format=raw` +
`-device virtio-blk-pci,disable-modern=on,drive=blk0`, then asserts the
ordered serial markers `BLK: 1` (PCI found) → `BLK: 2` (device ready) →
`BLK: 3` (2-sector read at LBA 64 verified byte-for-byte against the
build-time pattern). Failure diagnostics: `BLK: 5` = wait timeout, `BLK: 6`
= completion rejected, `BLK: 7` = bad args, `BLK: 8` = pattern mismatch.

## Design decisions

- **Direct Curlee, no C**: the issue's language-gap check — `addr_of`
  (issue #286), runtime `phys_read_u8/u16/u32/u64` + `phys_write_*`
  (curlee #279), module-level `static` + `[T; N]` arrays (curlee #278),
  `--define` sizing (curlee #296) — are all closed and proven by
  virtio_net.curlee; the blk driver uses the same surface with zero C.
- **Distinct names in the merged TU**: `blk_*` / `virtio_blk_*` everywhere
  (the merged TU forbids duplicate functions/externs; `pci_config_read32`,
  `vring_*_base`, `curlee_sfence` are virtio_net's, so the blk module owns
  `blk_pci_config_read32` + `blk_*` geometry helpers). `curlee_sfence` is
  declared ONCE by virtio_net.curlee in the merged TU; the standalone check
  uses a generated wrapper (`build/blk-check.curlee`, the Makefile rule).
- **Explicit array initializer lists are NOT parsed** (verified at this
  workspace's commit — only the `[0; N]` repeat form), so the verify
  pattern table is zero-initialized + filled once by `blk_pat_init()`.
- **The "JOE-BLK!" pattern's first byte is 74 ('J'), not 72 ('H')** — the
  smoke gate caught the ASCII transcription error live (72 would print
  "HOE-BLK!"); the fix is documented in `blk_pat_init`.

## Files

| File | Change |
|------|--------|
| `kernel/virtio_blk.curlee` | NEW: the driver (probe, init, read, sector-byte, err) |
| `kernel/kernel.curlee` | `serial_blk_marker`, `blk_pat_init`/`blk_pattern_byte`/`blk_pattern_ok`, `blk_bringup`; called from `main` after `net_bringup` |
| `scripts/make-blk-disk.sh` | NEW: build-side raw disk image + test pattern / model blob at a known LBA |
| `scripts/build-kernel.sh` | merge `virtio_blk.curlee` (after `virtio_net.curlee`) |
| `Makefile` | `BLK_QMEM_PAGES`/`BLK_BUF_SECTORS` defines; `blk-check.curlee` wrapper + check; `blk-disk.img` + `joeos-blk.iso` + `qemu-blk-smoke` targets; verify-gate nm checks |
| `.github/workflows/ci.yml` + `release.yml` | wire `qemu-blk-smoke` into CI + release gates |
| `docs/phase2-architecture.md`, `README.md` | roadmap row 2g + storage feature/limitation updates |

## Acceptance criteria

1. A virtio-blk device is discovered and initialized under QEMU. ✅ (`BLK: 1`, `BLK: 2`)
2. Raw sector reads land in a provided buffer, verified against the build-time pattern. ✅ (`BLK: 3` — 2 sectors at LBA 64, byte-for-byte)
3. A new QEMU smoke gate proves it end-to-end. ✅ (`make qemu-blk-smoke`)
4. Existing gates stay green (`check`, `verify`, `qemu-smoke`, `qemu-fb-smoke`, `qemu-net-smoke`).

## References

- virtio spec: legacy virtio-blk (0.9.5) I/O BAR registers + split virtqueue;
  transitional/legacy PCI IDs `0x1042`/`0x1001`, modern-only `0x1FE8`
- `plans/phase2d-1-virtio-net.md` — the transport/ring/sizing decisions this
  phase mirrors
- `docs/phase2f-report.md` §4 — the PVH no-PCI finding that restricts the
  disk to the GRUB/ISO path
- `docs/phase2c-report.md` §4.3 — the PVH LOAD budget motivating the
  per-build stub sizing
