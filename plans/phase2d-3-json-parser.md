# Phase 2d-3: Pure Curlee JSON/tool-call parser + VM tests

Parent: [#3 Phase 2d: LLM bridge (VirtIO-net / TCP + JSON)](https://github.com/w4ffl35/joeos/issues/3)
Depends on: 2d-2 (response buffer API `net_response_len`/`net_response_byte`)
Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) (**LOCKED — the
only authoritative envelope; parse exactly what 2d-4's stub emits**)
Status: OPEN — spec (filed as GitHub issue #7)

## Summary

A **pure, verified, VM-tested JSON parser in Curlee** for the LLM response
shape — the tool-call object defined in
[`docs/phase2d-wire.md`](../docs/phase2d-wire.md). It parses the fixed
response envelope (`{"tool":"frame_tick","args":[0,1,2]}`), extracts the
tool-call fields as **flat Int/Bool values**, and feeds the kernel tool-call
queue (`fb_tool_enqueue`) so 2d-4's end-to-end can prove the round-trip.

## Current state (what exists)

- The pure module pattern is established: `canvas.curlee`/`glyphs.curlee`/
  `assets.curlee` (contract-carrying where Z3 can prove it, contract-less +
  VM-asserted where non-linear/division is needed), merged into the single TU
  by `scripts/build-kernel.sh`, verified by `make check`, VM-tested by
  `make canvas-run` (`kernel/canvas_test.curlee`).
- The tool-call queue API exists: `fb_tool_enqueue(kind, arg)` in
  `kernel/fb.c`, ring geometry in `assets.curlee`
  (`tool_queue_slots`/`tool_slot_bytes`/`tool_queue_slot`), consumed live in
  `kernel/kernel.curlee` `main` (kind 1 = "frame tick").
- **Verifier fragment** (documented in `docs/phase2-architecture.md` §1 and
  the module headers): no struct params in contracted fns, no division in
  contracted fns, no calls-as-call-arguments, no `%`/shifts/bitwise — so a
  JSON parser must be **contract-less** (it needs string scanning, division,
  and per-char decisions) and **VM-asserted** instead, or model the tool call
  as flat Int/Bool payloads. We do BOTH (flat payloads + pure scanner),
  matching the issue body's explicit allowance.

## Goal

A pure `json.curlee` module (merged into the kernel TU like the others) plus
a VM test module, delivering:

1. **Response-byte model**: the parser reads the response one byte at a time
   via the 2d-2 externs (`net_response_len`/`net_response_byte`) — but as a
   PURE module it must be VM-testable, so the byte source is abstracted:
   the module exposes `json_parse(byte_at: ...)`-style helpers that take the
   byte stream as *parameters* (a small pure harness feeds a static test
   payload). Concretely: pure functions like
   `json_next_char(buf, pos)` / `json_skip_ws(buf, pos)` / `json_match_str(...)`
   that operate on an abstracted byte source, VM-asserted against literal
   payloads.
2. **Fixed tool-call shape (LOCKED in the wire doc)**: parse exactly the
   envelope in [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) into flat
   fields: `tool_name_len`/`tool_name_byte(i)` (the name, 1..15 bytes),
   `arg_count` (1..8), `arg_value(i)` (flat Int args in
   `-2147483648..2147483647`). Response ≤ 256 bytes. Out-of-shape input →
   the wire doc's deterministic `parse_error` codes (0..9), never a hang.
3. **Feed the queue**: a thin bridge (in `kernel.curlee` or the driver)
   calls `fb_tool_enqueue(kind, arg)` with the parsed fields — kind 2 =
   "LLM tool call", arg = the parsed value — proving the LLM bridge reaches
   the 2b tool-call queue (issue #3's end-to-end criterion). On
   `parse_error != 0` the bridge emits `JSON: E<code>` and does NOT enqueue.
4. **VM tests**: `json_test.curlee` (mirroring `canvas_test.curlee`)
   asserts the parser against the locked envelope, the allowed variation
   (whitespace, bounded tool/args), all malformed-error codes, and edge
   cases; `make json-run` gate.

## Wire shape (LOCKED — single source of truth)

The exact request body, response envelope, allowed variation, absolute
limits, and malformed-response error codes (0..9) are defined in
[`docs/phase2d-wire.md`](../docs/phase2d-wire.md) and are NOT repeated here.
The parser MUST implement exactly that file: the 36-byte envelope
`{"tool":"frame_tick","args":[0,1,2]}` is the happy path; the wire doc's
limit table (response ≤ 256 B, tool 1..15, args 1..8, arg range) bounds the
scan; the error-code table maps every malformed input to a deterministic
code. 2d-4's stub emits exactly this shape — no ambiguity remains.

## Design decisions

- **Contract-less parser, VM-asserted** — the issue body explicitly allows
  this ("keep JSON parsing contract-less and VM-asserted, or model
  tool-calls as flat Int/Bool payloads"). Do BOTH: flat payloads + pure
  scanner functions; correctness proven by `make json-run`, not Z3.
- **No string type**: bytes are Ints; "strings" are (length, byte-at-index)
  pairs — matches `net_response_byte` and the no-`String` rule.
- **Determinism + bounded scanning**: the parser consumes at most
  `min(response_len, 256)` bytes (wire-doc limit); a malformed payload
  yields the wire doc's error code, no loop.
- **Keep it merge-safe**: no `import`, no `main` in `json.curlee`; merged by
  `build-kernel.sh` in dependency order (after assets, before kernel).

## Files

| File | Change |
|------|--------|
| `kernel/json.curlee` | NEW: pure parser (scanner, ws-skip, match, arg extraction) implementing the wire-doc envelope |
| `kernel/json_test.curlee` | NEW: VM test — asserts the locked shape, variation, and all error codes |
| `scripts/build-kernel.sh` | Add `json.curlee` to the merge module list (order: canvas, glyphs, assets, json, kernel) |
| `Makefile` | `json-run` target (`curlee run kernel/json_test.curlee`); add to `verify`; `check` gains `json.curlee` |
| `kernel/kernel.curlee` | Externs for the byte source + `fb_tool_enqueue` bridge with kind 2; serial markers `JSON: 1` / `JSON: E<code>` |
| `docs/phase2-architecture.md` | Roadmap row 2d status + module list (follows the 2a/2b/2c report pattern) |

## Extern surface (added to kernel.curlee by this sub-issue)

```curlee
// From 2d-2 (net_stack): the response byte source (per the wire doc).
extern fn net_response_len() -> Int;
extern fn net_response_byte(i: Int) -> Int;
```

(No new externs live IN json.curlee — it stays pure; the bridge lives in
kernel.curlee.)

## Acceptance criteria

1. `make json-run` passes: the parser extracts `tool = "frame_tick"` and
   `args = [0,1,2]` from the locked 36-byte envelope, handles the wire doc's
   allowed variation (whitespace, bounded tool/args), returns the wire doc's
   deterministic error codes (0..9) on malformed input, and the whole assert
   suite returns 0.
2. `make check` passes with `json.curlee` added (contract-less module
   verifies; kernel.curlee bridge still verifies).
3. `make kernel` + `make verify` link both ELF paths with the merged json
   module.
4. End-to-end ready: `fb_tool_enqueue(2, ...)` is reachable from the parsed
   fields (proven live in 2d-4's smoke gate via serial `JSON: 1`);
   `parse_error != 0` emits `JSON: E<code>` and never enqueues.
5. All existing gates stay green.

## References

- Issue #3 body (§3 JSON module; "keep JSON parsing contract-less and
  VM-asserted, or model tool-calls as flat Int/Bool payloads")
- Wire shape: [`docs/phase2d-wire.md`](../docs/phase2d-wire.md) — the ONLY
  envelope this parser implements (request/response, limits, error codes,
  markers)
- `docs/phase2-architecture.md` §1 (verifier constraint table), §4 (module
  API pattern), §5 (`build-kernel.sh` merge)
- `kernel/canvas_test.curlee` — the VM-assert pattern to mirror
- Curlee stdlib `python` module (stubbed) as the interop reference:
  `~/Projects/curlee/stdlib/v1/std/python.curlee`
