# Phase 2d-2: Minimal TCP/IP stack (ARP/IP/TCP, fixed-slot buffers)

Parent: [#3 Phase 2d: LLM bridge (VirtIO-net / TCP + JSON)](https://github.com/w4ffl35/joeos/issues/3)
Depends on: 2d-1 (VirtIO-net driver) — consumes its RX/TX buffer API
Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) (single source of truth)
Status: OPEN — spec (filed as GitHub issue #6)

## Summary

A tiny, deterministic network stack in freestanding C: **ARP** (resolve the
gateway MAC), **IPv4** (parse + checksum), and **TCP** (fixed-slot
connection state, one connection to the host LLM server). Option B (raw
transport) is explicitly dropped — see "Design decision (LOCKED)" below. No
malloc, no dynamic buffers; every slot is a static array, bounded and
documented.

## Current state (what exists)

- Nothing network-related. 2d-1 will provide `net_rx_len()/net_tx_send()`
  etc. as the byte transport. This layer sits between that transport and the
  Curlee JSON/tool layer (2d-3).
- Host side target: a llama.cpp HTTP server (`llama-server -m <model>
  --port 8080`) on the QEMU host. **Routing (LOCKED):** QEMU user-net gives
  the guest `10.0.2.15/24` with gateway `10.0.2.2`; slirp's `10.0.2.2`
  gateway alias reaches host loopback services DIRECTLY. So the host server
  listening on `127.0.0.1:8080` (or `0.0.0.0:8080`) is reachable from the
  guest at `10.0.2.2:8080` — **no `hostfwd` is needed for the kernel → host
  direction** (hostfwd is only for host → guest, which the smoke path does
  not use).

## Goal

The kernel can open exactly **one** TCP connection to `10.0.2.2:8080`, send a
request, and receive the response — deterministically, with serial markers
proving each hop.

1. **ARP**: resolve `10.0.2.2` → MAC (send ARP request, parse reply, cache).
   Deterministic under QEMU user-net: slirp automatically answers ARP for
   the `10.0.2.2` gateway alias — the same mechanism 2d-1's smoke path uses
   for its first RX frame.
2. **IPv4**: build/parse IP headers, compute header checksums.
3. **TCP (minimal)**: fixed-slot connection state (one slot), handshake
   (SYN → SYN-ACK → ACK), send the fixed request (see the wire doc: exact
   36-byte JSON body), receive the response, no retransmission logic beyond
   a simple bounded retry (fuel-bounded loop, no infinite waits — mirrors
   the kernel's fuel-bounded `while` loops).
4. Expose a **byte-buffer API** to the Curlee layer: a static response buffer
   (≥ 256 B, per the wire doc's response limit) the JSON parser (2d-3) scans.

## Design decision (LOCKED)

Option A only: **raw Ethernet + ARP + IP + TCP one-shot** (~300-500 lines of
C), fully deterministic, real HTTP `POST /completion` to the host server.
Option B (skip TCP, port-forward a fixed UDP/raw payload) is DROPPED — it
moves protocol work into the harness (2d-4) and weakens the "kernel speaks
TCP" goal. If the TCP handshake explodes in scope, split the handshake into
its own sub-step but keep IP+ARP in 2d-2.

## Constraints (from the repo design laws)

- No libc, no malloc, no `String`/`Vec`; static buffers only.
- No threading, no interrupts assumed: the stack is **polled** — the kernel
  `main` loop (or a driver tick) calls `net_stack_poll()` per iteration;
  every step is fuel-bounded so the smoke gates stay timeout-safe.
- Deterministic verification lives in the Curlee layer; this C layer is
  verified by behavior in QEMU (serial markers), mirroring the fb.c pattern.
- PVH budget: same `JOE_PVH_BOOT` discipline as fb.c — if the stack's
  static buffers blow the PVH LOAD budget (~0x8000-0x10000 B memsz), compile
  them out on the PVH path and gate the network acceptance on the GRUB/ISO
  path. The response buffer is small (256 B) so it should fit; verify with
  `size`/`readelf -l` per the 2d-1 sizing rule.

## Files

| File | Change |
|------|--------|
| `kernel/net_stack.c` | NEW: ARP + IPv4 + minimal TCP (one connection, fixed slots) |
| `kernel/net_stack.h` | NEW: public API (`net_stack_poll`, `net_connect`, `net_send`, `net_response_len`, `net_response_ptr`) |
| `kernel/virtio_net.c` (2d-1) | Consumed: RX/TX buffer API; buffer ownership handoff (stack releases RX buffers) |
| `Makefile` | Compile `net_stack.o`; extend `qemu-net-smoke` to assert TCP markers |
| `kernel/kernel.curlee` | Externs + serial markers for each hop (`ARP: 1`, `TCP: 1`, `SND: <len>`, `RCV: <len>`) |

## Extern surface (Curlee window)

```curlee
extern fn net_connect(port: Int) -> Int;      // 1 = TCP connected to gw:port
extern fn net_send(buf_len: Int) -> Int;      // 1 = request queued (buffer pre-staged)
extern fn net_stack_poll() -> Int;            // advance stack; 1 = response ready
extern fn net_response_len() -> Int;          // bytes of the HTTP response body
extern fn net_response_byte(i: Int) -> Int;   // byte at index i (0..len-1)
```

`net_response_byte(i)` lets the pure Curlee JSON parser (2d-3) scan the
response one byte at a time — the verifier treats the bytes as opaque, so the
parser stays contract-less + VM-asserted, exactly the pattern the issue body
prescribes.

## Acceptance criteria

1. ARP resolves the QEMU gateway (serial `ARP: 1`) — the guest's ARP request
   for `10.0.2.2` draws slirp's automatic reply.
2. TCP handshake to `10.0.2.2:8080` completes (serial `TCP: 1`).
3. The fixed 36-byte request is sent and the response received
   (`SND: 36`/`RCV: 36` markers, per the wire doc) against a **deterministic
   host-side stub** (2d-4 provides the stub server; the real llama.cpp server
   is optional at this stage).
4. All steps are fuel-bounded and timeout-safe; no infinite waits.
5. Existing gates stay green (`check`, `canvas-run`, `qemu-smoke`,
   `qemu-fb-smoke`, `qemu-loop-smoke`, `qemu-net-smoke` from 2d-1).

## References

- Issue #3 body (§2 minimal TCP transport; llama.cpp server line)
- Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) — exact
  request body (36 bytes), response envelope (≤ 256 B), marker sequence
  (`ARP: 1` → `TCP: 1` → `SND:` → `RCV:`), extern names
- QEMU user-net: guest `10.0.2.15/24`, gateway `10.0.2.2`; `10.0.2.2`
  reaches host loopback directly (no hostfwd for kernel → host)
- RFC 826 (ARP), RFC 791 (IPv4), RFC 793 (TCP) — only the fixed, one-shot
  subset needed
