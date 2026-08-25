# Phase 2d-4: Host harness + end-to-end smoke gate

Parent: [#3 Phase 2d: LLM bridge (VirtIO-net / TCP + JSON)](https://github.com/w4ffl35/joeos/issues/3)
Depends on: 2d-1, 2d-2, 2d-3
Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) (**LOCKED — the
stub emits exactly this envelope; the gate asserts this marker sequence**)
Status: OPEN — spec (filed as GitHub issue #8)

## Summary

The host-side piece that makes the LLM round-trip **observable and
deterministic**: a harness that (a) starts a host server (the real
llama.cpp HTTP server, or a deterministic stub for CI), (b) launches QEMU
with the NIC attached, (c) collects the serial log, and (d) asserts the full
end-to-end marker sequence — proving the kernel sent a request, the host
responded with a tool call, the kernel parsed it and enqueued it into the 2b
tool-call queue, and rendered the acknowledgment frame.

## Current state (what exists)

- Host tooling: `scripts/find-curlee.sh`, `scripts/build_iso.sh`,
  `scripts/vbox-setup.sh`; Makefile gates `qemu-smoke` (serial grep),
  `qemu-fb-smoke` (`FB:` + `RING:`), `qemu-loop-smoke` (ordered
  `FR:0..FR:3, RING: 1, FB: 1, Hello World from JOE!`).
- The kernel tool-call queue exists (2b): `fb_tool_enqueue(kind, arg)`,
  8×16-byte ring in `kernel/fb.c`, geometry in `assets.curlee`; the loop
  already enqueues kind-1 "frame tick" intents.
- No host LLM integration, no NIC in any run target, no network smoke gate.

## Goal

`make qemu-llm-smoke` — a deterministic end-to-end gate:

1. **Host server**: a tiny Python stub (`scripts/llm_stub_server.py`) that
   listens on `127.0.0.1:8080` (bind `0.0.0.0:8080` for safety), accepts the
   kernel's request, and replies with the **locked tool-call envelope from
   [`docs/phase2d-wire.md`](../docs/phase2d-wire.md)**
   (`{"tool":"frame_tick","args":[0,1,2]}`). Deterministic, CI-safe, no
   model download. The real llama.cpp path is a documented variant
   (`LLM_SERVER=llama-server` env override), not the gate.
2. **QEMU launch**: boots the kernel (GRUB/ISO path with the NIC compiled
   in — see 2d-1's PVH sizing) with:
   ```
   -netdev user,id=n0 -device virtio-net-pci,disable-modern=on,netdev=n0
   ```
   **Routing (LOCKED): no `hostfwd` is needed for the kernel → host
   direction.** QEMU user-net gives the guest `10.0.2.15/24` with gateway
   `10.0.2.2`; slirp's `10.0.2.2` gateway alias reaches **host loopback
   services directly**, so the stub on `127.0.0.1:8080` is reachable from
   the guest at `10.0.2.2:8080`. (`hostfwd` would only forward host → guest,
   the wrong direction — it is omitted, or kept only as a documented
   host → guest debug aid, never part of the smoke path.)
   Serial captured to `build/serial-llm.log`, `-display none`,
   timeout-bounded.
3. **Kernel flow** (built on 2d-1/2/3): probe NIC → link up → ARP →
   TCP connect to `10.0.2.2:8080` → send the fixed 36-byte request (wire
   doc) → poll the stack → parse the response with the pure JSON module →
   enqueue a kind-2 tool call (`fb_tool_enqueue(2, arg)`) → render an
   acknowledgment frame → serial markers.
4. **Assertions**: the serial log must contain, in order (per the wire doc's
   marker sequence): `NET: 1..3`, `ARP: 1`, `TCP: 1`, `SND: 36`, `RCV: 36`,
   `JSON: 1`, `TOOL: 2` (queue enqueue of kind 2), `LLM: 1` (ack frame
   rendered) — plus the existing tail (`Hello World from JOE!`, halt). A
   `JSON: E<code>` marker fails the gate.
5. **Real-LLM variant** (optional, documented): run `llama-server -m
   <model> --port 8080` on the host and re-run the same gate; the stub and
   the real server both serve the same JSON shape (wire doc).

## Design decisions

- **Stub first, real LLM second**: CI determinism requires the stub. The
  real server is a manual/dev variant, never a CI gate (model download +
  nondeterministic completion text would break the smoke gate).
- **Wire shape is LOCKED in [`docs/phase2d-wire.md`](../docs/phase2d-wire.md)**
  (single source of truth): the stub emits exactly that envelope; the parser
  (2d-3) implements exactly that envelope; no inline copies in this spec, so
  the two cannot drift.
- **Ack frame**: the kernel renders a frame showing "LLM OK" (or a marker
  glyph) after the tool call is enqueued — this satisfies issue #3's
  "renders an acknowledgment frame". The serial `LLM: 1` marker is the
  gate; the visual is a bonus on the fb path.
- **Timeout discipline**: every step is fuel-bounded; the gate wraps QEMU
  in `timeout` (like `qemu-smoke`) so a hung NIC can never hang CI.
- **No-NIC-safe baseline**: the harness's no-NIC run (existing smoke gates)
  must stay green per 2d-1 criterion 6 — the gate suite never assumes the
  NIC is present.

## Files

| File | Change |
|------|--------|
| `scripts/llm_stub_server.py` | NEW: deterministic HTTP stub on :8080 (emits the wire-doc envelope) |
| `scripts/run-llm-smoke.sh` | NEW: start stub → launch QEMU (`-netdev user,id=n0` + `disable-modern=on`, no hostfwd) → assert ordered serial markers |
| `Makefile` | `qemu-llm-smoke` target invoking the harness; optional `LLM_SERVER` override for the real llama.cpp path |
| `docs/phase2d-wire.md` | EXISTS (created in this scoping round) — the locked request + response envelope shared by 2d-3 and 2d-4 |
| `docs/phase2d-report.md` | Report (mirrors `docs/phase2e-2-report.md` structure) |

## Acceptance criteria

1. `make qemu-llm-smoke` PASSES: serial log contains the full ordered
   sequence `NET: 1`, `NET: 2`, `NET: 3`, `RX: <len>` (the 2d-1 first-RX
   marker — the slirp ARP reply consumed by `net_bringup`, emitted between
   `NET: 3` and `ARP: 1` on the user-net smoke path; absent with no NIC),
   `ARP: 1`, `TCP: 1`, `SND: 36`, `RCV: 36`, `JSON: 1`, `TOOL: 2`, `LLM: 1`,
   `Hello World from JOE!` — the kernel → host stub → JSON parse → tool-queue
   enqueue → ack frame round-trip, deterministic and timeout-safe. A
   `JSON: E<code>` marker fails the gate.
2. `make qemu-llm-smoke` fails fast (non-zero) on a missing/mismatched
   marker (grep with exit code, mirroring `qemu-smoke`).
3. The real-llama.cpp variant is documented and runnable
   (`LLM_SERVER=... make qemu-llm-smoke` or a documented script step), not
   CI-gated.
4. All existing gates stay green (`check`, `canvas-run`, `json-run`,
   `qemu-smoke`, `qemu-fb-smoke`, `qemu-loop-smoke`, `qemu-net-smoke`),
   including the no-NIC runs (2d-1 criterion 6).

## References

- Issue #3 body (§4 host side, §5 end-to-end, acceptance criteria 2–5)
- Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) — locked
  request/response, marker sequence, extern names
- `docs/phase2e-2-report.md` — the report structure + gate-table pattern to
  mirror in `docs/phase2d-report.md`
- Makefile `qemu-smoke` / `qemu-loop-smoke` — the serial-grep + `timeout`
  gate pattern
- llama.cpp server: `llama-server -m <model> --port 8080`
- QEMU user-net: guest `10.0.2.15/24`, gateway `10.0.2.2` reaches host
  loopback directly (no hostfwd for kernel → host)
