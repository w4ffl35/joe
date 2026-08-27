// SPDX-License-Identifier: GPL-3.0
//
// virtio_net.c — RAW RING/BUFFER OWNER + BUILD-GEOMETRY SHIM for the Curlee
// VirtIO-net driver (gh issue #14). The 798 lines of driver logic (PCI
// config-space probe, legacy virtqueue setup, RX/TX, frame I/O) migrated to
// kernel/virtio_net.curlee (genuine Curlee functions, codegen'd as the static
// curlee_net_* symbols); the extern surface in kernel/net.h is now implemented
// there, not here.
//
// What remains, and ONLY why (per docs/c-boundary-policy.md §1 — "No logic in
// C — only I/O touches and raw memory moves"):
//   - The two large PVH-conditional ring-memory buffers (rx_qmem/tx_qmem,
//     5 pages each) + the RX/TX data buffers (2 x 2048 B each), compiled OUT
//     on the PVH build (JOE_PVH_BOOT): QEMU's `-kernel` PVH loader refuses
//     ELFs whose LOAD segment (file + BSS) exceeds a hard budget (verified
//     empirically, docs/phase2c-report.md §4.3). Curlee has no conditional
//     compilation and BOTH builds compile the SAME merged kernel.c
//     (scripts/build-kernel.sh), so a full-size Curlee static array would land
//     in the PVH kernel's BSS unconditionally. The C `#ifndef JOE_PVH_BOOT` is
//     the ONLY way to size them per build — the sole reason these buffers stay
//     in C (mirrors kernel/fb.c's asset region / frame ring, gh issue #13).
//   - The base-address getters (virtio_net_*_qmem_base / virtio_net_*_buf_base,
//     0 on the PVH 1-byte stubs) that let the Curlee layer address the C-owned
//     buffers as runtime addresses (the fb_asset_region_base_get pattern).
//   - The build discriminator (virtio_net_pvh_build).
//   - virtio_net_sfence(): the ring-publication store fence the Curlee layer
//     calls before each avail-idx store (an "I/O touch / raw memory move").
//     The C original's compiler barrier (net_mem_barrier) is redundant in the
//     codegen (every ring access is a volatile phys_write_* store, and GCC
//     never reorders volatile accesses); the sfence itself is kept as this
//     tiny extern so the QEMU TCG kick-ordering behavior is identical.
//
// No port I/O, no ring math, no protocol logic — the driver is Curlee.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Sizing (LOCKED in issue #5): option 2 = full rings on GRUB, option 1 =
// 1-byte stubs on the PVH build. The Curlee driver mirrors these constants as
// literals (kernel/virtio_net.curlee) — the qemu-net-smoke gate catches drift.
// ---------------------------------------------------------------------------
#ifndef JOE_PVH_BOOT
// OPTION 2 ACTIVE (GRUB path): full rings + buffers. RX buffers >= 256 B
// (wire-shape §3.2 absolute response limit); 2048 covers the ARP frame and
// the 2d-2 HTTP round trip comfortably.
#define NET_QMEM_PAGES 5     // >= 4095 + 3*4096 bytes, 4096-aligned base inside
#define NET_RX_BUFS     2
#define NET_TX_BUFS     2
#define NET_BUF_BYTES   2048
#else
// OPTION 1 ACTIVE (PVH path): 1-byte stubs. Every getter returns 0 and the
// Curlee driver's net_init() bails on virtio_net_pvh_build()==1 (see the
// .curlee header for the platform trigger).
#define NET_QMEM_PAGES 1
#define NET_RX_BUFS     1
#define NET_TX_BUFS     1
#define NET_BUF_BYTES   1
#endif

// Ring memory: 3 x 4096 per queue. QEMU aligns the used ring to 4096 relative
// to the queue base (align_up in its legacy vring code), so the BASE MUST be
// 4096-aligned. A bare __attribute__((aligned(4096))) on a static array is not
// reliably honored by the linker for BSS symbols, so we use an over-allocated
// buffer and the Curlee queue_setup() manually aligns the working base (the C
// original's `(raw + 4095) & ~4095UL` is ported there verbatim).
//
// Over-allocation math: the aligned base can be up to (4096-1) bytes past the
// raw start, and we need 3 x 4096 from the aligned base. So the raw buffer must
// be >= 4095 + 3*4096 = 16383 bytes. 5 pages (20480) is safe on both sides.
static unsigned char rx_qmem[NET_QMEM_PAGES * 4096];
static unsigned char tx_qmem[NET_QMEM_PAGES * 4096];

// RX/TX data buffers (static, no malloc). Addressed by the Curlee driver via
// the base getters; the descriptor addr field is the row address (like the C
// original's `(uint64_t)(unsigned long)rx_buf[s]`).
static unsigned char rx_buf[NET_RX_BUFS][NET_BUF_BYTES];
static unsigned char tx_buf[NET_TX_BUFS][NET_BUF_BYTES];

// ---------------------------------------------------------------------------
// Build geometry (the ONLY #ifdefs left): the Curlee driver reads
// virtio_net_pvh_build() to bail out on the PVH path, and the four base
// getters for the C-owned buffers' runtime addresses (0 on the PVH stub).
// ---------------------------------------------------------------------------

long long virtio_net_pvh_build(void)
{
#ifdef JOE_PVH_BOOT
    return 1;
#else
    return 0;
#endif
}

long long virtio_net_rx_qmem_base(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (long long)(unsigned long)rx_qmem;
#endif
}

long long virtio_net_tx_qmem_base(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (long long)(unsigned long)tx_qmem;
#endif
}

// Base address of RX buffer slot `slot` (0 on the PVH stub, or for an
// out-of-range slot — defense-in-depth over the Curlee bounds).
long long virtio_net_rx_buf_base(long long slot)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (slot >= 0 && slot < NET_RX_BUFS)
    {
        return (long long)(unsigned long)rx_buf[(unsigned int)slot];
    }
    return 0;
#endif
}

long long virtio_net_tx_buf_base(long long slot)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (slot >= 0 && slot < NET_TX_BUFS)
    {
        return (long long)(unsigned long)tx_buf[(unsigned int)slot];
    }
    return 0;
#endif
}

// Ring publication fence (see the file header): orders the desc[]/ring[]
// volatile stores before the avail-idx store the device reads. Kept as the C
// original's sfence so the QEMU TCG kick-ordering behavior is identical.
void virtio_net_sfence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}
