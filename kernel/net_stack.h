// SPDX-License-Identifier: GPL-3.0
//
// net_stack.h — Phase 2d-2 TCP/IP stack RAW-STATE SHIM API (kernel/net_stack.c).
//
// gh issue #12: the ARP/IPv4/TCP protocol logic (byte layout, checksums, HTTP
// response framing) moved to kernel/net_stack.curlee (pure, VM-verified) and
// the one-shot glue (net_connect / net_send / net_stack_poll /
// net_response_len / net_response_byte) moved to kernel/kernel.curlee as
// genuine Curlee functions. This file is the C side of that boundary: the
// mutable one-shot state + the 256-byte response body store that Curlee
// cannot own (no module-level globals, no arrays) — the vbe_state.c
// precedent (gh issue #11). No protocol logic, no checksums, no frame
// construction — only raw state and memory moves (docs/c-boundary-policy.md
// §1: "No logic in C — only I/O touches and raw memory moves.").
//
// The Curlee window into this API is declared in kernel/kernel.curlee with
// the exact extern names below.
//
// One-shot deterministic flow (LOCKED in issue #6 / docs/phase2d-wire.md §5):
//   ARP: 1   net_connect() resolves gateway 10.0.2.2 -> MAC (by CONSTANT:
//            slirp's 52:55:0a:00:02:02 — net_stack.curlee's gw_mac_byte; a
//            best-effort ARP announce is still sent so slirp learns the guest)
//   TCP: 1   the same call completes the handshake (SYN -> SYN-ACK -> ACK)
//   SND: 36  net_send() stages + queues the fixed 36-byte HTTP request
//   RCV: 36  net_stack_poll() advances the stack; response body received
//
// Phase values (net_state_get): 0 idle, 1 connected, 2 sent/in-flight,
// 3 response ready, 4 failed (terminal). Everything is polled and
// fuel-bounded: every RX wait is a bounded loop (net_rx_wait from 2d-1) and
// the Curlee-side response poll is bounded by the C-owned net_poll_fuel()
// counter — mirroring the kernel's fuel-bounded while loops.
//
// PVH sizing (LOCKED in the 2d-2 spec, mirrors fb.c/virtio_net.c): on the
// PVH build (JOE_PVH_BOOT) the response store compiles to 1 byte and every
// extern returns 0 / no-ops — the API surface is identical on both paths.
#ifndef JOE_NET_STACK_H
#define JOE_NET_STACK_H

// Implemented by kernel/net_stack.c (freestanding, no libc).
// One-shot phase (0 idle, 1 connected, 2 sent, 3 ready, 4 failed).
long long net_state_get(void);
void net_state_reset(long long port);   // fresh one-shot: src 49152, dst port
void net_state_connect(long long my_seq, long long peer_seq);  // -> 1
void net_state_sent(void);              // -> 2
void net_state_ready(void);             // -> 3
void net_state_failed(void);            // -> 4 (terminal, no retry)
long long net_my_seq(void);
long long net_peer_seq(void);
long long net_dst_port(void);
// Response body byte store (raw memory moves; the 256-byte wire-shape cap).
long long net_resp_len(void);           // stored bytes (0 when invalidated)
void net_resp_len_set(long long n);     // n < 0 invalidates (truncated path)
long long net_resp_byte(long long i);   // byte i (0 if OOB / not ready)
void net_resp_store(long long b);       // append one byte (bounded by the cap)
// Poll parse state (the Curlee packed http_step state, opaque to C — owned
// by the shim only because Curlee has no cross-call state).
long long net_poll_state(void);
void net_poll_state_set(long long st);
// Poll fuel: the C-owned counter that bounds the Curlee response loop
// (mirrors fb_loop_frame / net_poll_tick). Increments and returns the count.
long long net_poll_fuel(void);

#endif /* JOE_NET_STACK_H */
