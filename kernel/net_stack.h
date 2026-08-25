// SPDX-License-Identifier: GPL-3.0
//
// net_stack.h — Phase 2d-2 TCP/IP stack public API (kernel/net_stack.c).
//
// Layer: sits between the VirtIO-net byte transport (2d-1, kernel/virtio_net.c
// via kernel/net.h) and the Curlee JSON/tool layer (2d-3). The Curlee window
// into this API is declared in kernel/kernel.curlee with the exact extern
// names below; this header is the C side (implemented by net_stack.c,
// consuming net.h from virtio_net.c).
//
// One-shot deterministic flow (LOCKED in issue #6 / plans/phase2d-2-tcp-stack.md):
//   net_connect(8080)  -> 1 = ARP resolved 10.0.2.2 + TCP handshake complete
//   net_send(36)       -> 1 = fixed 36-byte HTTP request queued on the wire
//   net_stack_poll()   -> advance the stack (RX service + one state step);
//                          0 = still reading, 1 = response body fully received,
//                          2 = fuel exhausted / protocol error (failed closed),
//                          3 = truncated (Content-Length > 256 B; the body is
//                              invalidated — net_response_len() reports 0)
//   net_response_len() -> bytes of the HTTP response body (<= 256; 0 when not
//                          ready or truncated)
//   net_response_byte(i) -> byte i of the body (0..len-1)
//
// Only Content-Length-framed responses are supported (no chunked encoding).
//
// Everything is polled and fuel-bounded: every RX wait is a bounded loop
// (net_rx_wait from 2d-1) and the Curlee-side response poll is bounded by the
// C-owned net_poll_tick() counter — mirroring the kernel's fuel-bounded while
// loops. No malloc, no dynamic buffers; every buffer is a static array.
//
// PVH sizing (LOCKED in the 2d-2 spec, mirrors fb.c/virtio_net.c): on the PVH
// build (JOE_PVH_BOOT) the stack's static buffers compile down to 1 byte and
// every extern returns 0 — the API surface is identical on both paths, and
// the network acceptance gate boots the GRUB/ISO path where the full stack
// runs (the PVH machine exposes no legacy PCI config space, so the NIC is
// unreachable there anyway).
#ifndef JOE_NET_STACK_H
#define JOE_NET_STACK_H

// Implemented by kernel/net_stack.c (freestanding, no libc).
long long net_connect(long long port);
long long net_send(long long buf_len);
long long net_stack_poll(void);
long long net_response_len(void);
long long net_response_byte(long long i);

// C-side convenience accessor (not part of the Curlee extern surface — 2d-3
// reads bytes through net_response_byte): pointer to the response body, or 0
// when no response is ready.
const unsigned char* net_response_ptr(void);

#endif /* JOE_NET_STACK_H */
