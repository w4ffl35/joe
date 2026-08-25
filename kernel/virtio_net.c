// SPDX-License-Identifier: GPL-3.0
//
// virtio_net.c — Phase 2d-1 VirtIO-net driver (RX/TX ring, init, link-up).
//
// Boot path & transport (LOCKED in issue #5 / plans/phase2d-1-virtio-net.md):
//   Legacy virtio 0.9.5 over the PCI I/O BAR. The smoke path boots QEMU with
//   `-device virtio-net-pci,disable-modern=on,netdev=n0`, which makes the
//   device present the legacy ID 0x1000 and the legacy-only register layout
//   (QueuePFN / legacy status / device config at BAR0+0x14). No capability
//   walk, no MSI-X, no modern registers. The driver NEVER offers
//   VIRTIO_F_VERSION_1 (bit 32) — impossible anyway, the legacy guest-features
//   register is 32 bits wide — which is what keeps the device in legacy mode.
//
// Smoke frame source (LOCKED in issue #5, adapted for QEMU 10): the plan's
// slirp auto-ARP-answer (guest ARPs gateway 10.0.2.2 -> slirp replies) does
// not fire on QEMU 10.0.11 user-net in practice (verified empirically). The
// deterministic replacement keeps the same guest-ARP mechanism but uses a
// second QEMU instance connected via a socket netdev: the sender's broadcast
// ARP request is delivered by the socket backend to the receiver's NIC, which
// reports RX: <len>. No host-side injection, no live network. See the
// qemu-net-smoke Makefile target.
//
// Ring layout (matches Linux vring_init + QEMU legacy exactly):
//   queue base (4096-aligned) -> PFN register = phys >> 12
//     page 0: vring_desc[256]   (16 B each)
//     page 1: vring_avail       (flags + idx + ring[256])
//     page 2: vring_used        (flags + idx + ring[256] of {id,len})
//   There is no GuestPageSize register in the shipped legacy layout: Linux
//   (virtio_pci_legacy.c, VIRTIO_PCI_QUEUE_ADDR_SHIFT 12) and QEMU both fix
//   the PFN unit at 4096 bytes. The issue's mention is a draft-era register.
//
// PVH sizing mode (LOCKED option 2 default, option 1 fallback):
//   GRUB build (no JOE_PVH_BOOT) — OPTION 2 ACTIVE: full rings (2 RX x 2048 B
//   + 2 TX x 2048 B, ring depth 256). The NIC runs on the GRUB/ISO path, which
//   is where qemu-net-smoke boots.
//   PVH build (JOE_PVH_BOOT) — OPTION 1 ACTIVE: rings/buffers stub to 1 byte,
//   every extern returns 0, and net_probe() is a safe no-op. Trigger: the
//   documented Phase 2f measurement (docs/phase2f-report.md §4) that QEMU's
//   `-kernel` PVH machine (xenpvh) exposes NO legacy PCI config space — every
//   0xCF8/0xCFC read returns 0 regardless of machine type or device — so the
//   NIC is unreachable there, and stubbing keeps the image inside the PVH LOAD
//   budget. The extern surface is identical on both paths (mirror fb.c's
//   region/ring pattern). All existing gates stay green with the driver
//   compiled IN and no NIC present.
//
// No-NIC-safe (acceptance criterion 6): with no virtio-net device, net_probe()
// finds nothing (config reads return 0xFFFFFFFF), net_init()/net_link_up()/
// net_rx_len()/net_tx_send() all return 0, and boot continues to VGA + serial
// + halt. No hang, no crash.
//
// Trust model (mirrors Phys<T> discipline): port I/O and ring DMA are raw,
// trusted access gated by this driver being the only toucher. The RX handoff
// contract (net.h) prevents buffer reuse while the stack owns a frame.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Port I/O — freestanding, no libc sys/io.h. Mirrors kernel/putc_driver.c and
// kernel/vbe.c. This is the ONLY section of the driver that touches I/O ports.
// ---------------------------------------------------------------------------
static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(unsigned short port, unsigned short v)
{
    __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(unsigned short port, unsigned int v)
{
    __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// ---------------------------------------------------------------------------
// PCI config space (legacy ports 0xCF8/0xCFC)
// ---------------------------------------------------------------------------
#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

#define PCI_VENDOR_VIRTIO       0x1AF4
#define PCI_DEV_LEGACY_NET      0x1000  // legacy net (disable-modern=on)
#define PCI_DEV_MODERN_NET      0x1041  // modern transitional net
#define PCI_DEV_MODERN_ONLY_NET 0x1FE9  // modern-only net (recognized, not inited)
#define PCI_DEV_BLOCK           0x1001  // block device — NEVER matched by this driver

static unsigned int pci_config_read32(unsigned int bus, unsigned int dev,
                                      unsigned int func, unsigned int off)
{
    const unsigned int addr = 0x80000000u | (bus << 16) | (dev << 11) |
                              (func << 8) | (off & 0xFCu);
    outl(PCI_ADDR_PORT, addr);
    return inl(PCI_DATA_PORT);
}

// ---------------------------------------------------------------------------
// Legacy virtio-pci I/O BAR registers (virtio 0.9.5)
// ---------------------------------------------------------------------------
#define VIRTIO_PCI_HOST_FEATURES 0x00  // r  (32-bit)
#define VIRTIO_PCI_GUEST_FEATURES 0x04 // w  (32-bit)
#define VIRTIO_PCI_QUEUE_PFN      0x08 // rw (32-bit)
#define VIRTIO_PCI_QUEUE_NUM      0x0C // r  (16-bit)
#define VIRTIO_PCI_QUEUE_SEL      0x0E // w  (16-bit)
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10 // w  (16-bit)
#define VIRTIO_PCI_STATUS         0x12 // rw (8-bit)
#define VIRTIO_PCI_ISR            0x13 // r  (8-bit)

// Legacy device config starts at BAR0 + 20 (0x14) when MSI-X is unused
// (Linux virtio_pci_legacy.c: VIRTIO_PCI_CONFIG_OFF(0) == 20).
#define VIRTIO_PCI_CONFIG_OFF 20

#define VIRTIO_CONFIG_S_ACKNOWLEDGE 0x01
#define VIRTIO_CONFIG_S_DRIVER      0x02
#define VIRTIO_CONFIG_S_DRIVER_OK   0x04
#define VIRTIO_CONFIG_S_FAILED      0x80

#define VIRTIO_NET_F_MAC    5
#define VIRTIO_NET_F_STATUS 16
// VIRTIO_F_VERSION_1 (bit 32) is NEVER offered — see file header.
//
// Virtio-net header handling (LOCKED, 2d-4 finding):
//   The driver does NOT negotiate any csum/gso offload feature, but QEMU's
//   virtio-net still expects a `struct virtio_net_hdr` (10 bytes) at the START
//   of every TX buffer, and writes one at the start of every RX buffer. This
//   was verified empirically (QEMU 10.0.11): a TX frame staged without the
//   header reaches the network with its first 10 bytes (the Ethernet dst MAC
//   and half the src MAC) consumed as the header — slirp sees `34:56:08:06:00:01`
//   instead of `ff:ff:ff:ff:ff:ff` and drops it (the documented 2d-4 blocker).
//   The qemu-net-smoke socket gate never caught this because BOTH QEMUs use
//   virtio-net (symmetric header consumption) and it only greps the RX length.
//
//   The driver therefore stages the 10-byte header (all zeros — a plain,
//   unoffloaded segment) before each Ethernet frame on TX, and skips the
//   header on RX. The Curlee/net_stack layer above continues to see pure
//   Ethernet frames (byte 0 = dst MAC), exactly as it always assumed.
#define VIRTIO_NET_HDR_BYTES 10

#define VIRTIO_NET_S_LINK_UP 0x01

// virtio-net device config fields (at BAR0 + VIRTIO_PCI_CONFIG_OFF + field).
#define VIRTIO_NET_CONFIG_MAC    0  // 6 bytes
#define VIRTIO_NET_CONFIG_STATUS 6  // 2 bytes

#define VIRTIO_PCI_VRING_ALIGN 4096

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VIRTIO_QUEUE_RX 0
#define VIRTIO_QUEUE_TX 1

// ---------------------------------------------------------------------------
// Sizing (LOCKED in issue #5): option 2 = full rings on GRUB, option 1 =
// 1-byte stubs on the PVH build. All static, no malloc anywhere.
// ---------------------------------------------------------------------------
#ifndef JOE_PVH_BOOT
// OPTION 2 ACTIVE (GRUB path): full rings + buffers. RX buffers >= 256 B
// (wire-shape §3.2 absolute response limit); 2048 covers the ARP frame and
// the 2d-2 HTTP round trip comfortably.
#define NET_RING_NUM    256   // QEMU virtio-net legacy queue depth
#define NET_RX_BUFS     2
#define NET_TX_BUFS     2
#define NET_BUF_BYTES   2048
#define NET_RX_POLL_MAX 20000000  // bounded poll in net_rx_wait (never hangs)
#else
// OPTION 1 ACTIVE (PVH path): 1-byte stubs. The extern surface is identical;
// every entry point returns 0 (see file header for the platform trigger).
#define NET_RING_NUM    1
#define NET_RX_BUFS     1
#define NET_TX_BUFS     1
#define NET_BUF_BYTES   1
#define NET_RX_POLL_MAX 0
#endif

// Ring memory: 3 x 4096 per queue. QEMU aligns the used ring to 4096 relative
// to the queue base (align_up in its legacy vring code), so the BASE MUST be
// 4096-aligned. A bare __attribute__((aligned(4096))) on a static array is not
// reliably honored by the linker for BSS symbols, so we use an over-allocated
// buffer and manually align the working pointer inside queue_setup().
//
// Over-allocation math: the aligned base can be up to (4096-1) bytes past the
// raw start, and we need 3 x 4096 from the aligned base. So the raw buffer must
// be >= 4095 + 3*4096 = 16383 bytes. 5 pages (20480) is safe on both sides.
#ifdef JOE_PVH_BOOT
static unsigned char rx_qmem[1];
static unsigned char tx_qmem[1];
#else
#define NET_QMEM_PAGES 5
static unsigned char rx_qmem[NET_QMEM_PAGES * VIRTIO_PCI_VRING_ALIGN];
static unsigned char tx_qmem[NET_QMEM_PAGES * VIRTIO_PCI_VRING_ALIGN];
#endif

// RX/TX data buffers (static, no malloc).
static unsigned char rx_buf[NET_RX_BUFS][NET_BUF_BYTES];
static unsigned char tx_buf[NET_TX_BUFS][NET_BUF_BYTES];

// Legacy split virtqueue ring structs (fixed-width, no padding surprises).
typedef struct net_vring_desc {
    uint64_t addr;      // physical address of the buffer
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} net_vring_desc;

typedef struct net_vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[NET_RING_NUM];
} net_vring_avail;

typedef struct net_vring_used_elem {
    uint32_t id;
    uint32_t len;
} net_vring_used_elem;

typedef struct net_vring_used {
    uint16_t flags;
    uint16_t idx;
    net_vring_used_elem ring[NET_RING_NUM];
} net_vring_used;

// ---------------------------------------------------------------------------
// Driver state (all static, .bss — writable on both boot paths)
// ---------------------------------------------------------------------------
static unsigned short io_base = 0;   // legacy I/O BAR base of the NIC
static unsigned char pci_found = 0;  // net_probe() cache
static unsigned char net_ready = 0;  // net_init() completed (rings live)
static unsigned char net_status_feature = 0; // VIRTIO_NET_F_STATUS negotiated
static unsigned char net_link_up_now = 0;
static unsigned char guest_mac[6];
static unsigned char mac_known = 0;

// Desc allocation: RX uses desc ids 0..NET_RX_BUFS-1, TX uses
// NET_RX_BUFS..NET_RX_BUFS+NET_TX_BUFS-1. Static split, no free-list.
static net_vring_desc* rx_desc = 0;
static net_vring_avail* rx_avail = 0;
static net_vring_used* rx_used = 0;
static net_vring_desc* tx_desc = 0;
static net_vring_avail* tx_avail = 0;
static net_vring_used* tx_used = 0;

static uint16_t rx_avail_head = 0;   // number of RX descs added to the avail ring
static uint16_t tx_avail_head = 0;
static uint16_t rx_used_last = 0;    // used-ring idx we have consumed
static uint16_t tx_used_last = 0;

// rx_buf_state: 0 = free (re-armable), 1 = armed on ring, 2 = frame ready.
// The stack owns a frame from the first net_rx_len() > 0 until net_rx_done().
static unsigned char rx_buf_state[NET_RX_BUFS];
static uint16_t rx_frame_len[NET_RX_BUFS];
static int rx_current = -1;          // slot of the frame the stack reads now

static unsigned char tx_buf_state[NET_TX_BUFS];  // 0 = free, 1 = in flight
static unsigned char tx_stage = 0;               // slot the stack stages into

static long long poll_ticks = 0;     // net_poll_tick() fuel (C-owned counter)

static void net_zero(void* p, unsigned long n)
{
    unsigned char* d = (unsigned char*)p;
    while (n > 0)
    {
        *d++ = 0;
        --n;
    }
}

static void net_mem_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

// Stronger barrier for ring publication: sfence orders the desc/avail stores
// before the idx store the device reads. x86 stores are already ordered, but
// QEMU's TCG doesn't always model the outw kick as a full fence, so make the
// ordering explicit.
static void net_sfence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

// ---------------------------------------------------------------------------
// Virtqueue helpers
// ---------------------------------------------------------------------------

// Point the ring struct pointers at the 3-page queue memory for qsel.
static void ring_pointers(unsigned int qsel, unsigned char* qmem)
{
    net_vring_desc* d = (net_vring_desc*)(void*)(qmem + 0 * VIRTIO_PCI_VRING_ALIGN);
    net_vring_avail* a = (net_vring_avail*)(void*)(qmem + 1 * VIRTIO_PCI_VRING_ALIGN);
    net_vring_used* u = (net_vring_used*)(void*)(qmem + 2 * VIRTIO_PCI_VRING_ALIGN);
    if (qsel == VIRTIO_QUEUE_RX)
    {
        rx_desc = d;
        rx_avail = a;
        rx_used = u;
    }
    else
    {
        tx_desc = d;
        tx_avail = a;
        tx_used = u;
    }
}

// Program one legacy virtqueue: select, read QUEUE_NUM, zero the 3 pages,
// write QueuePFN = phys >> 12, read back, install ring pointers. `qmem` is a
// 4-page over-allocated buffer; the working base is the first 4096-aligned
// address inside it (QEMU aligns the used ring to 4096 relative to this base,
// so base misalignment would put desc/avail/used on the wrong pages).
static int queue_setup(unsigned int qsel, unsigned char* qmem)
{
    outw(io_base + VIRTIO_PCI_QUEUE_SEL, (unsigned short)qsel);
    const unsigned int num = inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (num == 0)
    {
        return 0;
    }
    if (num > NET_RING_NUM)
    {
        return 0;  // device wants more than we sized for — unsupported
    }
    // Manual 4096-alignment of the working base (see ring-memory comment).
    const unsigned long raw = (unsigned long)qmem;
    const unsigned long base =
        (raw + (VIRTIO_PCI_VRING_ALIGN - 1)) & ~(unsigned long)(VIRTIO_PCI_VRING_ALIGN - 1);
    net_zero((unsigned char*)base, 3 * VIRTIO_PCI_VRING_ALIGN);
    outl(io_base + VIRTIO_PCI_QUEUE_PFN, (unsigned int)(base >> 12));
    if (inl(io_base + VIRTIO_PCI_QUEUE_PFN) != (unsigned int)(base >> 12))
    {
        return 0;
    }
    ring_pointers(qsel, (unsigned char*)base);
    return 1;
}

// Kick the device: new buffers are available on queue qsel.
static void queue_notify(unsigned int qsel)
{
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, (unsigned short)qsel);
}

// Arm every free RX buffer on the avail ring (fixed slot <-> desc id 1:1).
static void refill_rx(void)
{
    unsigned int armed = 0;
    unsigned int s;
    for (s = 0; s < NET_RX_BUFS; ++s)
    {
        if (rx_buf_state[s] == 0)
        {
            rx_desc[s].addr = (uint64_t)(unsigned long)rx_buf[s];
            rx_desc[s].len = NET_BUF_BYTES;
            rx_desc[s].flags = VRING_DESC_F_WRITE;  // device writes into RX
            rx_avail->ring[rx_avail_head % NET_RING_NUM] = (uint16_t)s;
            ++rx_avail_head;
            rx_buf_state[s] = 1;
            ++armed;
        }
    }
    if (armed > 0)
    {
        net_sfence();  // desc[] + ring[] stores visible before idx
        rx_avail->idx = rx_avail_head;
        net_mem_barrier();
        queue_notify(VIRTIO_QUEUE_RX);
    }
}

// Collect completed RX frames from the used ring into the per-slot state.
static void service_rx_used(void)
{
    while (rx_used->idx != rx_used_last)
    {
        const net_vring_used_elem* e = &rx_used->ring[rx_used_last % NET_RING_NUM];
        const unsigned int id = e->id;
        if (id < NET_RX_BUFS && rx_buf_state[id] == 1)
        {
            rx_frame_len[id] = (uint16_t)e->len;
            rx_buf_state[id] = 2;  // frame ready (owned by the stack)
        }
        ++rx_used_last;
    }
}

// Reclaim completed TX frames from the used ring.
static void service_tx_used(void)
{
    while (tx_used->idx != tx_used_last)
    {
        const net_vring_used_elem* e = &tx_used->ring[tx_used_last % NET_RING_NUM];
        const unsigned int id = e->id;
        if (id >= NET_RX_BUFS && id < NET_RX_BUFS + NET_TX_BUFS)
        {
            tx_buf_state[id - NET_RX_BUFS] = 0;  // free again
        }
        ++tx_used_last;
    }
}

// ---------------------------------------------------------------------------
// Extern surface (Curlee window + 2d-2 C API, see kernel/net.h)
// ---------------------------------------------------------------------------

// 1 = a legacy-capable virtio-net PCI device was found (I/O BAR readable).
long long net_probe(void)
{
    if (pci_found)
    {
        return 1;
    }
    // Legacy PCI: bus 0 only. Nonexistent devices read 0xFFFFFFFF — safe.
    unsigned int dev;
    for (dev = 0; dev < 32; ++dev)
    {
        unsigned int func;
        for (func = 0; func < 8; ++func)
        {
            const unsigned int id = pci_config_read32(0, dev, func, 0);
            if (id == 0xFFFFFFFFu || id == 0)
            {
                continue;
            }
            const unsigned int vendor = id & 0xFFFFu;
            const unsigned int device = (id >> 16) & 0xFFFFu;
            if (vendor != PCI_VENDOR_VIRTIO)
            {
                continue;
            }
            if (device == PCI_DEV_LEGACY_NET || device == PCI_DEV_MODERN_NET)
            {
                const unsigned int bar = pci_config_read32(0, dev, func, 0x10);
                if ((bar & 1u) == 0)
                {
                    // MMIO BAR (modern transport). Recognized but not driven on
                    // the legacy smoke path (documented fallback; the smoke
                    // path forces legacy with disable-modern=on -> I/O BAR).
                    continue;
                }
                const unsigned short io = (unsigned short)(bar & 0xFFFCu);
                if (io == 0)
                {
                    continue;  // BAR not assigned — not usable
                }
                io_base = io;
                pci_found = 1;
                return 1;
            }
            // 0x1FE9 (modern-only) and 0x1001 (block) are intentionally never
            // initialized here: 0x1FE9 has no legacy transport, 0x1001 is not
            // a net device.
        }
    }
    return 0;
}

// 1 = device ready (rings set up, RX armed). Safe no-op when no NIC.
long long net_init(void)
{
    if (net_ready)
    {
        return 1;
    }
    if (!pci_found)
    {
        if (net_probe() != 1)
        {
            return 0;
        }
    }
#ifdef JOE_PVH_BOOT
    return 0;  // option 1 stub (see file header)
#else
    // 1. Reset: status = 0, then read back (QEMU resets synchronously).
    outb(io_base + VIRTIO_PCI_STATUS, 0);
    {
        unsigned int guard = 0;
        while (inb(io_base + VIRTIO_PCI_STATUS) != 0 && guard < 100)
        {
            ++guard;
        }
    }
    // 2. Host features -> offer MAC + STATUS, NEVER VERSION_1 (32-bit reg).
    const unsigned int host = inl(io_base + VIRTIO_PCI_HOST_FEATURES);
    unsigned int guest = 0;
    if (host & (1u << VIRTIO_NET_F_MAC))
    {
        guest |= (1u << VIRTIO_NET_F_MAC);
    }
    if (host & (1u << VIRTIO_NET_F_STATUS))
    {
        guest |= (1u << VIRTIO_NET_F_STATUS);
        net_status_feature = 1;
    }
    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, guest);
    net_mem_barrier();
    // 3. Status: ACKNOWLEDGE | DRIVER.
    outb(io_base + VIRTIO_PCI_STATUS,
         VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
    // 4. Set up RX and TX queues (3-page legacy layout each).
    if (!queue_setup(VIRTIO_QUEUE_RX, rx_qmem) ||
        !queue_setup(VIRTIO_QUEUE_TX, tx_qmem))
    {
        outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_CONFIG_S_FAILED);
        return 0;
    }
    // 5. Driver OK — device is live.
    outb(io_base + VIRTIO_PCI_STATUS,
         VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
             VIRTIO_CONFIG_S_DRIVER_OK);
    // 6. Read the device config (MAC + link status) from BAR0 + 0x14.
    {
        unsigned int i;
        for (i = 0; i < 6; ++i)
        {
            guest_mac[i] = inb(io_base + VIRTIO_PCI_CONFIG_OFF +
                               VIRTIO_NET_CONFIG_MAC + i);
        }
        mac_known = 1;
        if (net_status_feature)
        {
            const unsigned short st =
                (unsigned short)(inw(io_base + VIRTIO_PCI_CONFIG_OFF +
                                     VIRTIO_NET_CONFIG_STATUS));
            net_link_up_now = (st & VIRTIO_NET_S_LINK_UP) ? 1 : 0;
        }
        else
        {
            // No STATUS feature: nothing to read. QEMU user-net is always up,
            // so 2d-2's link check can proceed (documented assumption).
            net_link_up_now = 1;
        }
    }
    // 7. Arm both RX buffers and kick.
    net_ready = 1;
    refill_rx();
    return 1;
#endif
}

// 1 = link up (VIRTIO_NET_S_LINK_UP from the negotiated STATUS feature).
long long net_link_up(void)
{
    return net_ready ? (long long)net_link_up_now : 0;
}

// Bytes in the current RX frame (0 = none). Also advances the used ring so a
// frame that arrived between polls is collected.
//
// The device writes the 10-byte virtio-net header at the START of the RX
// buffer (see the file-header comment), so the Ethernet frame the stack reads
// starts at byte VIRTIO_NET_HDR_BYTES. The reported length is the Ethernet
// frame length (used-ring len minus the header).
long long net_rx_len(void)
{
    if (!net_ready)
    {
        return 0;
    }
    service_rx_used();
    if (rx_current < 0 && rx_buf_state[0] == 2)
    {
        rx_current = 0;
    }
    else if (rx_current < 0 && NET_RX_BUFS > 1 && rx_buf_state[1] == 2)
    {
        rx_current = 1;
    }
    if (rx_current < 0)
    {
        return 0;
    }
    if (rx_frame_len[rx_current] <= VIRTIO_NET_HDR_BYTES)
    {
        return 0;  // header-only or undersized — nothing usable
    }
    return (long long)(rx_frame_len[rx_current] - VIRTIO_NET_HDR_BYTES);
}

// Byte i of the held RX frame (0 if none/out of range). 2d-2's byte source.
// Skips the virtio-net header so the stack sees pure Ethernet (byte 0 = dst
// MAC), exactly as it always assumed.
long long net_rx_byte(long long i)
{
    if (!net_ready || rx_current < 0 || i < 0 ||
        i >= (long long)(rx_frame_len[rx_current] - VIRTIO_NET_HDR_BYTES))
    {
        return 0;
    }
    return (long long)rx_buf[rx_current][(unsigned int)(i + VIRTIO_NET_HDR_BYTES)];
}

// Release the held RX frame; the driver reclaims + re-arms the buffer.
void net_rx_done(void)
{
    if (!net_ready || rx_current < 0)
    {
        return;
    }
    rx_buf_state[rx_current] = 0;  // free -> refill re-arms it
    rx_current = -1;
    refill_rx();
}

// Stage byte b at offset i of the TX staging buffer. 1 = ok, 0 = bad index.
long long net_tx_stage_byte(long long i, long long b)
{
    if (!net_ready || i < 0 || i >= NET_BUF_BYTES || b < 0 || b > 255)
    {
        return 0;
    }
    tx_buf[tx_stage][(unsigned int)i] = (unsigned char)b;
    return 1;
}

// Queue the staged frame. 1 = queued, 0 = no free TX buffer / not ready.
//
// The stack stages the PURE Ethernet frame at offset 0..len-1; the virtio-net
// header (10 zero bytes, see the file-header comment) is prepended HERE so the
// device sees header + Ethernet on the ring. The descriptor length includes
// the header (len + 10); the frame content is unchanged for the stack.
long long net_tx_send(long long len)
{
    if (!net_ready || len <= 0 || len + VIRTIO_NET_HDR_BYTES > NET_BUF_BYTES)
    {
        return 0;
    }
    if (tx_buf_state[tx_stage] != 0)
    {
        return 0;  // no free TX slot
    }
    // Shift the staged Ethernet frame up by the header size, then zero the
    // header slot. (The stack staged bytes 0..len-1; the wire needs
    // [header][frame].)
    {
        long long i;
        for (i = len - 1; i >= 0; --i)
        {
            tx_buf[tx_stage][(unsigned int)(i + VIRTIO_NET_HDR_BYTES)] =
                tx_buf[tx_stage][(unsigned int)i];
        }
        for (i = 0; i < VIRTIO_NET_HDR_BYTES; ++i)
        {
            tx_buf[tx_stage][(unsigned int)i] = 0;
        }
    }
    const unsigned int desc_id = NET_RX_BUFS + tx_stage;
    tx_desc[desc_id].addr = (uint64_t)(unsigned long)tx_buf[tx_stage];
    tx_desc[desc_id].len = (uint32_t)(len + VIRTIO_NET_HDR_BYTES);
    tx_desc[desc_id].flags = 0;  // device reads the TX buffer
    net_sfence();  // desc[] + ring[] stores visible before idx
    tx_avail->ring[tx_avail_head % NET_RING_NUM] = (uint16_t)desc_id;
    ++tx_avail_head;
    tx_buf_state[tx_stage] = 1;
    tx_stage = (unsigned char)((tx_stage + 1) % NET_TX_BUFS);
    net_mem_barrier();
    tx_avail->idx = tx_avail_head;
    net_mem_barrier();
    queue_notify(VIRTIO_QUEUE_TX);
    return 1;
}

// Byte i of the negotiated guest MAC (0 if unknown). QEMU user-net guests
// use 10.0.2.15 with this MAC as the ARP sender.
long long net_mac_byte(long long i)
{
    if (!mac_known || i < 0 || i > 5)
    {
        return 0;
    }
    return (long long)guest_mac[(unsigned int)i];
}

// Advance the rings and return the C-owned tick counter (bounded-poll fuel).
long long net_poll_tick(void)
{
    if (net_ready)
    {
        service_rx_used();
        service_tx_used();
    }
    return ++poll_ticks;
}

// Bounded wait for the first RX frame (deterministic, never hangs): polls the
// used ring up to NET_RX_POLL_MAX iterations, yielding to QEMU's main loop via
// an ISR port read every 256 iterations (a pure memory spin can starve in TCG).
// Returns net_rx_len() when a frame arrives, else 0.
long long net_rx_wait(void)
{
    if (!net_ready)
    {
        return 0;
    }
    long long i;
    for (i = 0; i < NET_RX_POLL_MAX; ++i)
    {
        if ((i & 0xFF) == 0)
        {
            (void)inb(io_base + VIRTIO_PCI_ISR);  // yield to the QEMU main loop
            // Re-notify RX in case the original kick raced the idx publish.
            queue_notify(VIRTIO_QUEUE_RX);
        }
        service_rx_used();
        // Also reclaim completed TX frames while we wait: the one-shot TCP
        // stack (net_stack.c) may have queued the ARP request on this wait
        // (2d-4 keeps that optional path), and both TX slots must be free for
        // the SYN that follows. Without this, a timed-out ARP wait leaves both
        // TX buffers "in flight" forever and net_tx_send() for the SYN fails
        // with "no free TX slot" — the documented 2d-4 blocker.
        service_tx_used();
        if (rx_buf_state[0] == 2 || (NET_RX_BUFS > 1 && rx_buf_state[1] == 2))
        {
            return net_rx_len();
        }
        __asm__ volatile("pause");
    }
    return 0;
}

// Build + send the deterministic ARP request for the slirp gateway 10.0.2.2
// (the LOCKED frame-injection mechanism: slirp auto-answers, delivering the
// first RX frame). Frame = Ethernet II (14 B) + ARP request (28 B) = 42 B.
// Returns net_tx_send() result.
long long net_arp_request_gateway(void)
{
    long long i;
    if (!net_ready)
    {
        return 0;
    }
    // Ethernet II header.
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(i, 0xFF);            // dst = broadcast
        net_tx_stage_byte(6 + i, net_mac_byte(i));  // src = our MAC
    }
    net_tx_stage_byte(12, 0x08);               // ethertype = ARP (0x0806)
    net_tx_stage_byte(13, 0x06);
    // ARP header.
    net_tx_stage_byte(14, 0x00);               // htype = Ethernet
    net_tx_stage_byte(15, 0x01);
    net_tx_stage_byte(16, 0x08);               // ptype = IPv4
    net_tx_stage_byte(17, 0x00);
    net_tx_stage_byte(18, 6);                  // hlen
    net_tx_stage_byte(19, 4);                  // plen
    net_tx_stage_byte(20, 0x00);               // op = request
    net_tx_stage_byte(21, 0x01);
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(22 + i, net_mac_byte(i));  // sha
    }
    net_tx_stage_byte(28, 10);                 // spa = 10.0.2.15 (slirp guest)
    net_tx_stage_byte(29, 0);
    net_tx_stage_byte(30, 2);
    net_tx_stage_byte(31, 15);
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(32 + i, 0);          // tha = 00:00:00:00:00:00
    }
    net_tx_stage_byte(38, 10);                 // tpa = 10.0.2.2 (slirp gateway)
    net_tx_stage_byte(39, 0);
    net_tx_stage_byte(40, 2);
    net_tx_stage_byte(41, 2);
    // Pad to the 60-byte Ethernet minimum (42 -> 60). Slirp can drop
    // sub-minimum frames; 18 zero pad bytes keep the frame valid.
    {
        long long p;
        for (p = 42; p < 60; ++p)
        {
            net_tx_stage_byte(p, 0);
        }
    }
    return net_tx_send(60);                    // 14 + 28 + 18 pad = 60 bytes
}
