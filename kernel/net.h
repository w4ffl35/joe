// SPDX-License-Identifier: GPL-3.0
//
// net.h — shared net constants + RX/TX buffer API for the Phase 2d bridge.
//
// Single source of truth for the byte-buffer contract between the NIC driver
// (2d-1) and the TCP layer (2d-2). Since gh issue #14 the driver is GENUINE
// Curlee (kernel/virtio_net.curlee — codegen'd as the static curlee_net_*
// symbols), and gh issue #296 DELETED the last C residual (kernel/virtio_net.c):
// the ring/buffer memory is Curlee statics sized per build via `--define`, the
// base-address getters are Curlee addr_of reads, and the ring-publication
// fence is the runtime's curlee_sfence. The Curlee window into the API is the
// same function set as the declarations below (threaded with `pm: cap
// phys.mem` in Curlee, dropped by codegen). The declarations remain as the
// C-visible contract documentation; the 2d-2 stack consumes the surface
// through kernel/net_glue.curlee.
//
// RX handoff (fixed-slot, driver -> stack):
//   net_rx_len()          bytes in the CURRENT held RX frame (0 = none)
//   net_rx_byte(i)        byte i of the held frame (0 if out of range)
//   net_rx_done()         release the held frame; the driver reclaims the
//                         buffer and re-arms it on the RX ring
// TX staging (stack -> driver):
//   net_tx_stage_byte(i,b) stage byte b at offset i of the TX buffer
//   net_tx_send(len)      queue the staged frame; 1 = queued, 0 = no buffer
// Link / probe:
//   net_probe()          1 = virtio-net PCI device found (legacy-capable)
//   net_init()           1 = device ready (rings set up, RX armed)
//   net_link_up()        1 = link up (VIRTIO_NET_S_LINK_UP negotiated)
//   net_mac_byte(i)      byte i of the negotiated guest MAC (0 if unknown)
//   net_poll_tick()      dev-loop tick counter (bounded-poll fuel; the C side
//                        owns the mutable counter, like fb_loop_frame)
//
// Ownership rule (LOCKED in the 2d-1 spec): the driver NEVER reuses an RX
// buffer the stack is still parsing. The stack owns a frame from the first
// net_rx_len() > 0 until net_rx_done(). The driver reclaims only released
// buffers and re-arms them on the ring. Fixed slots, no malloc anywhere.
//
// Buffer sizing (LOCKED in issue #5): 2 RX buffers x 2048 B + 2 TX buffers x
// 2048 B. The RX buffers must be >= 256 B (wire-shape §3.2 absolute limit for
// the LLM response body, docs/phase2d-wire.md). On the PVH build
// (JOE_PVH_BOOT) the Curlee statics stub down to 1 element via the PVH
// `--define` set and every base getter returns 0 (gh issue #296) — the API
// surface is identical on both paths.
#ifndef JOE_NET_H
#define JOE_NET_H

#define NET_RX_BUFS      2
#define NET_RX_BUF_BYTES 2048
#define NET_TX_BUFS      2
#define NET_TX_BUF_BYTES 2048

// Implemented by kernel/virtio_net.curlee (codegen'd as curlee_net_* / the
// Curlee addr_of getters; kernel/virtio_net.c is DELETED, gh issue #296).
long long net_probe(void);
long long net_init(void);
long long net_link_up(void);
long long net_rx_len(void);
long long net_rx_byte(long long i);
void net_rx_done(void);
long long net_tx_stage_byte(long long i, long long b);
long long net_tx_send(long long len);
long long net_mac_byte(long long i);
long long net_poll_tick(void);
// Deterministic smoke helpers (declared here per the net.h contract; used by
// the 2d-2 stack): bounded RX wait + the gateway ARP request.
long long net_rx_wait(void);
long long net_arp_request_gateway(void);
 
 #endif /* JOE_NET_H */
