#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
#
# llm_stub_server.py — Phase 2d-4 deterministic host LLM stub (GitHub issue #8).
#
# Purpose: the CI-safe stand-in for the real llama.cpp HTTP server. It listens
# on 127.0.0.1:8080 (bound to 0.0.0.0 so QEMU user-net's gateway alias
# 10.0.2.2 can reach it — see docs/phase2d-wire.md §1), accepts the kernel's
# fixed HTTP POST, and replies with the LOCKED tool-call envelope:
#
#   {"tool":"frame_tick","args":[0,1,2]}
#
# (exactly 36 bytes, docs/phase2d-wire.md §3). This is the same JSON shape the
# real llama.cpp server serves in the documented LLM_SERVER variant, so the
# parser and the gate cannot drift between the stub and the real path.
#
# Determinism contract (acceptance criterion 1): the response body is a fixed
# constant — no model, no randomness, no nondeterministic completion text. The
# smoke gate asserts the full marker sequence; a stub that emitted anything
# else would fail the gate immediately.
#
# HTTP framing (must match the kernel's parser, kernel/net_stack.c):
#   - HTTP/1.1 200 OK
#   - Content-Type: application/json
#   - Content-Length: <body length>   (required — the 2d-2 stack ONLY supports
#     Content-Length framing; chunked transfer-encoding is NOT decoded and
#     would hit the 256-byte truncation path, see net_stack.c)
#   - Connection: close               (the kernel sends Connection: close)
#   - blank line, then the 36-byte JSON body (no trailing newline)
#
# Usage:
#   python3 scripts/llm_stub_server.py [--port 8080] [--host 0.0.0.0]
#
# It runs until interrupted (Ctrl-C) or killed; the smoke harness starts it in
# the background and terminates it after the QEMU run.

import argparse
import socket
import sys

# The LOCKED envelope (docs/phase2d-wire.md §3) — byte-for-byte, 36 bytes.
ENVELOPE = b'{"tool":"frame_tick","args":[0,1,2]}'

# The HTTP response the kernel's 2d-2 stack parses (net_stack.c): 200 + JSON
# content type + Content-Length framing + Connection: close, then the body.
# The body has NO trailing newline so the body length equals the wire doc's
# 36-byte envelope exactly (net_response_len() == 36, marker "RCV: 36").
def build_response():
    body = ENVELOPE
    headers = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n"
        b"\r\n"
    )
    return headers + body


def handle_client(conn, log):
    """Read the request headers (we don't need the body — the kernel sends a
    fixed 36-byte JSON request), then write the fixed response."""
    try:
        # Read the request head. The kernel's POST is small (~150 bytes total);
        # reading until the blank line (\r\n\r\n) is enough. Bounded read so a
        # misbehaving peer can't pin the connection forever.
        conn.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 8192:
            chunk = conn.recv(1024)
            if not chunk:
                break
            data += chunk
        log.write("llm-stub: request head: %s\n" % data[:200].decode("latin-1").replace("\r", "\\r").replace("\n", "\\n"))
        log.flush()
        conn.sendall(build_response())
    except socket.timeout:
        log.write("llm-stub: timeout waiting for request head\n")
        log.flush()
    except OSError as e:
        log.write("llm-stub: connection error: %s\n" % e)
        log.flush()
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description="JOE Phase 2d-4 deterministic LLM stub")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="listen port (default 8080)")
    parser.add_argument("--log", default=None, help="optional log file for request/response diagnostics")
    args = parser.parse_args()

    log = open(args.log, "w", buffering=1) if args.log else sys.stdout
    log.write("llm-stub: listening on %s:%d, envelope=%s (%d bytes)\n"
              % (args.host, args.port, ENVELOPE.decode("ascii"), len(ENVELOPE)))
    log.flush()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)

    try:
        while True:
            conn, addr = srv.accept()
            log.write("llm-stub: connection from %s\n" % (addr,))
            log.flush()
            handle_client(conn, log)
    except KeyboardInterrupt:
        log.write("llm-stub: shutting down\n")
        log.flush()
    finally:
        srv.close()
        if args.log:
            log.close()


if __name__ == "__main__":
    main()
