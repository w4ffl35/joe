// SPDX-License-Identifier: GPL-3.0
//
// net_stack.c — RAW STATE + BYTE-STORE SHIM for the Curlee TCP/IP stack
// (gh issue #12). The 1056 lines of ARP/IPv4/TCP protocol logic migrated to
// kernel/net_stack.curlee (pure, VM-verified) + kernel/kernel.curlee (the
// one-shot glue); what remains here is ONLY the mutable state and the
// response body store that Curlee cannot own (no module-level globals, no
// arrays — verified language gaps, documented in net_stack.curlee's header):
//   - the one-shot connection state (ports, seqs, phase) + poll fuel,
//   - the 256-byte response body byte store (raw memory moves),
//   - the poll parse-state slot (the Curlee packed http_step state, opaque).
// No checksums, no frame construction, no header parsing — per
// docs/c-boundary-policy.md §1 ("No logic in C — only I/O touches and raw
// memory moves"), the vbe_state.c precedent (gh issue #11).
//
// One-shot flow (LOCKED in issue #6 / docs/phase2d-wire.md §5):
//   ARP: 1   net_connect() resolves gateway 10.0.2.2 -> MAC (by CONSTANT:
//            slirp's 52:55:0a:00:02:02 — net_stack.curlee's gw_mac_byte)
//   TCP: 1   the same call completes the handshake (SYN -> SYN-ACK -> ACK)
//   SND: 36  net_send() stages + queues the fixed 36-byte HTTP request
//   RCV: 36  net_stack_poll() advances the stack; response body received
//
// Phase values (net_state_get): 0 idle, 1 connected, 2 sent/in-flight,
// 3 response ready, 4 failed (terminal). The glue (kernel.curlee) owns the
// state machine; this file only stores and reports it.
//
// PVH sizing (LOCKED in the 2d-2 spec): on the PVH build (JOE_PVH_BOOT) the
// response store compiles to 1 byte and every extern returns 0 / no-ops —
// the API surface is identical on both paths (mirrors fb.c / virtio_net.c).
#include <stddef.h>
#include <stdint.h>
#include "net_stack.h"

#ifndef JOE_PVH_BOOT
// Response body buffer (wire-shape §3.2: <= 256 B).
#define NET_STACK_RESP_BODY 256
#else
#define NET_STACK_RESP_BODY 1
#endif

static unsigned char ns_resp_body[NET_STACK_RESP_BODY];

// Mutable one-shot state (owned here — Curlee has no globals).
// Phase: 0 idle, 1 connected, 2 sent/in-flight, 3 response ready, 4 failed.
static long long ns_state = 0;
static unsigned int ns_my_seq = 0;
static unsigned int ns_peer_seq = 0;
static unsigned short ns_src_port = 0;   // our ephemeral port (fixed 49152)
static unsigned short ns_dst_port = 0;
static long long ns_resp_len = 0;        // stored body bytes; -1 = invalidated
static long long ns_http_state = 0;      // Curlee packed http_step state (opaque)
static long long ns_polls = 0;           // net_stack_poll() fuel counter

long long net_state_get(void)
{
    return ns_state;
}

void net_state_reset(long long port)
{
    ns_state = 0;
    ns_my_seq = 0;
    ns_peer_seq = 0;
    ns_src_port = 49152;   // fixed ephemeral port (one-shot; FIX 4, 2d-2 review)
    ns_dst_port = (unsigned short)port;
    ns_resp_len = 0;
    ns_http_state = 0;
    ns_polls = 0;
}

void net_state_connect(long long my_seq, long long peer_seq)
{
    ns_my_seq = (unsigned int)my_seq;
    ns_peer_seq = (unsigned int)peer_seq;
    ns_state = 1;
}

void net_state_sent(void) { ns_state = 2; }
void net_state_ready(void) { ns_state = 3; }
void net_state_failed(void) { ns_state = 4; }

long long net_my_seq(void) { return ns_my_seq; }
long long net_peer_seq(void) { return ns_peer_seq; }
long long net_dst_port(void) { return ns_dst_port; }

// 0 when the store was invalidated (the truncation path set it to -1), so a
// truncated body never masquerades as a valid response (C semantics kept).
long long net_resp_len(void)
{
    return (ns_resp_len < 0) ? 0 : ns_resp_len;
}

void net_resp_len_set(long long n) { ns_resp_len = n; }

long long net_resp_byte(long long i)
{
    if (ns_resp_len < 0 || i < 0 || i >= ns_resp_len)
    {
        return 0;
    }
    return (long long)ns_resp_body[(unsigned int)i];
}

// Append one body byte (raw memory move). Bounded by the 256-byte cap and
// gated on the store not being invalidated — the Curlee http_step already
// enforces the same cap, so this is defense-in-depth.
void net_resp_store(long long b)
{
    if (ns_resp_len >= 0 && ns_resp_len < NET_STACK_RESP_BODY)
    {
        ns_resp_body[(unsigned int)ns_resp_len] = (unsigned char)b;
        ++ns_resp_len;
    }
}

long long net_poll_state(void) { return ns_http_state; }
void net_poll_state_set(long long st) { ns_http_state = st; }

long long net_poll_fuel(void) { return ++ns_polls; }
