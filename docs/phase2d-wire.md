# Phase 2d — Wire shape (single source of truth)

Parent: [#3 Phase 2d: LLM bridge (VirtIO-net / TCP + JSON)](https://github.com/w4ffl35/joeos/issues/3)
Status: **LOCKED** — this file is the authoritative wire shape for the LLM
bridge. Sub-issues [2d-3](../plans/phase2d-3-json-parser.md) (parser) and
[2d-4](../plans/phase2d-4-host-harness.md) (harness/stub) MUST implement
exactly this envelope. Any change to the shape requires updating this file
first, then both sub-issues — never the reverse.

---

## 1. Overview

The kernel sends one fixed HTTP request to the host LLM server and receives
one fixed JSON response. The host server is reachable from the guest at
`10.0.2.2:8080` (QEMU user-net gateway alias — see the harness spec for the
routing explanation; **no `hostfwd` is needed for the kernel → host
direction**).

| Side | Endpoint | Body |
|------|----------|------|
| Kernel → host | `POST http://10.0.2.2:8080/completion` | fixed JSON request (below) |
| Host → kernel | HTTP 200, `Content-Type: application/json` | fixed JSON response (below) |

## 2. Kernel request (exact JSON string the kernel sends)

The kernel transmits this exact byte string (no trailing newline, no extra
whitespace — the bytes are fixed so the TX path is deterministic and the
stub can match it exactly):

```json
{"tool":"frame_tick","args":[0,1,2]}
```

Byte length: **36** (verify: `printf '%s' '{"tool":"frame_tick","args":[0,1,2]}' | wc -c` → 36).

- Field names are exact: `tool`, `args`.
- `tool` is the string literal `frame_tick` (no quotes inside, lowercase,
  underscore).
- `args` is an array of exactly three decimal integers: `0`, `1`, `2`
  (no whitespace inside the array).
- This is a **JSON value**, not an HTTP form — the `Content-Type` header is
  `application/json`.

## 3. Host response (exact envelope the stub emits / the parser accepts)

The host server responds with this exact JSON body (the stub emits exactly
this; the real llama.cpp server is configured/proxied to serve the same
inner shape):

```json
{"tool":"frame_tick","args":[0,1,2]}
```

Byte length: **36** (same string as the request — the round-trip is
symmetric for the smoke gate; the real LLM may produce a different `tool`
name or args, but they MUST follow the same shape and stay within the
limits below).

### 3.1 Allowed variation (what the parser must handle)

The parser (2d-3) is written against the fixed shape but must tolerate the
following bounded variation:

- **Whitespace** between JSON tokens (spaces, tabs, newlines): the scanner
  skips it. `{ "tool" : "frame_tick" , "args" : [ 0 , 1 , 2 ] }` is valid.
- **`tool` value**: any ASCII string of length 1..15 (letters, digits,
  `_`). The parser returns it as `tool_name_len` + `tool_name_byte(i)`.
- **`args`**: an array of 1..8 integers, each in `-2147483648..2147483647`
  (fits a signed 32-bit range; `Int` is 64-bit so no overflow). The parser
  returns `arg_count` (1..8) + `arg_value(i)`.
- **Field order**: `tool` first, then `args` — the parser may reject any
  other order (simplest) or accept it (scanner-based); the stub always emits
  `tool` first. Rejecting other orders is acceptable and deterministic.

### 3.2 Absolute limits (hard, shared by 2d-3 and 2d-4)

| Limit | Value |
|-------|-------|
| Response byte length | ≤ 256 bytes |
| `tool` name length | 1..15 bytes |
| `arg_count` | 1..8 |
| `arg_value` range | `-2147483648..2147483647` |
| Request byte length (kernel TX) | exactly 36 bytes |

These limits are baked into the static RX buffer sizing (2d-1: RX buffers ≥
256 B) and the parser's bounded scan (2d-3: never reads past
`net_response_len()`).

## 4. Malformed-response error codes (returned by the parser, 2d-3)

The parser is contract-less and never hangs. It returns a single Int
`parse_error` code:

| Code | Meaning |
|------|---------|
| `0` | OK — parsed successfully; `tool_name_len`/`arg_count` valid |
| `1` | Response too long (> 256 bytes) |
| `2` | Not a JSON object (first non-ws char is not `{`) |
| `3` | Missing `tool` field, or it is not a JSON string |
| `4` | `tool` value length out of range (0 or > 15) |
| `5` | Missing `args` field, or it is not a JSON array |
| `6` | `arg_count` out of range (0 or > 8) |
| `7` | An arg is not a JSON integer, or out of `-2147483648..2147483647` |
| `8` | Trailing garbage after the closing `}` of the object |
| `9` | Generic structural error (unexpected token / premature end) |

The kernel bridge (2d-3) maps `parse_error != 0` to a serial error marker
(`JSON: E<code>`) instead of enqueueing; the smoke gate asserts `JSON: 1`
only on code `0`.

## 5. Marker sequence (shared by 2d-1..2d-4 gates)

The end-to-end smoke gate (`make qemu-llm-smoke`, 2d-4) asserts this exact
ordered serial sequence:

```
NET: 1        # 2d-1: PCI probe found virtio-net
NET: 2        # 2d-1: device initialized (rings set up)
NET: 3        # 2d-1: link up
RX: <len>     # 2d-1: first RX frame — the slirp ARP reply consumed by
              #       net_bringup (deterministic on the user-net smoke path;
              #       absent with no NIC). Empirically 60-64 bytes (padded
              #       ARP reply, minus/plus the virtio-net header discipline).
ARP: 1        # 2d-2: gateway 10.0.2.2 resolved
TCP: 1        # 2d-2: TCP connection to 10.0.2.2:8080 established
SND: 36       # 2d-2: request (36 bytes) sent
RCV: 36       # 2d-2: response (36 bytes) received
JSON: 1       # 2d-3: parser OK (parse_error == 0)
TOOL: 2       # 2d-3: fb_tool_enqueue(2, arg) succeeded (kind 2)
LLM: 1        # 2d-4: ack frame rendered
Hello World from JOE!   # existing serial tail (after the GRUB 60 FPS loop
                        # markers FR:/RING:/FB: on the fb boot path)
```

Each marker is emitted by the sub-issue named in the right column; the
sub-issue specs must agree with this sequence and with the extern names
listed below.

## 6. Extern surface (consolidated, shared by 2d-1..2d-4)

| Extern | Owner | Meaning |
|--------|-------|---------|
| `net_probe() -> Int` | 2d-1 | 1 = virtio-net PCI device found |
| `net_init() -> Int` | 2d-1 | 1 = device ready (rings set up) |
| `net_link_up() -> Int` | 2d-1 | 1 = link up |
| `net_rx_len() -> Int` | 2d-1 | bytes in the current RX frame (0 = none) |
| `net_rx_done() -> Unit` | 2d-1 | release the current RX buffer |
| `net_tx_send(len: Int) -> Int` | 2d-1 | 1 = queued, 0 = no buffer |
| `net_connect(port: Int) -> Int` | 2d-2 | 1 = TCP connected to gw:port |
| `net_send(buf_len: Int) -> Int` | 2d-2 | 1 = request queued (buffer pre-staged) |
| `net_stack_poll() -> Int` | 2d-2 | advance stack; 1 = response ready |
| `net_response_len() -> Int` | 2d-2 | bytes of the HTTP response body |
| `net_response_byte(i: Int) -> Int` | 2d-2 | byte at index i (0..len-1) |

`net_response_byte(i)` is the ONLY byte source the parser reads — 2d-3 stays
pure by abstracting this behind parameterized scanner helpers (see the 2d-3
spec); the bridge in `kernel.curlee` wires the extern to the scanner.

## 7. Change policy

1. Edit this file FIRST if the shape must change.
2. Update the 2d-3 parser spec and the 2d-4 harness spec to match (they
   reference this file — no inline copies of the envelope).
3. Update the stub (`scripts/llm_stub_server.py`) and the parser together in
   the same change; the smoke gate catches drift.
