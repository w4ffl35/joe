// SPDX-License-Identifier: GPL-3.0
//
// net_stack.c — Phase 2d-2 minimal TCP/IP stack (ARP + IPv4 + one-shot TCP).
//
// Layer: sits between the VirtIO-net byte transport (2d-1, kernel/virtio_net.c
// via kernel/net.h) and the Curlee JSON/tool layer (2d-3). The Curlee window
// into this API is declared in kernel/kernel.curlee (net_connect / net_send /
// net_stack_poll / net_response_len / net_response_byte); this file is the C
// side, mirroring the fb.c pattern (deterministic behavior verified by serial
// markers in QEMU, not by the Curlee verifier — the verifier stays on the pure
// data flow, exactly as the 2d-2 spec prescribes).
//
// Deterministic one-shot flow (LOCKED in issue #6 / docs/phase2d-wire.md §5):
//   ARP: 1   net_connect() resolves gateway 10.0.2.2 -> MAC (ARP request +
//            reply, via 2d-1's fuel-bounded net_rx_wait)
//   TCP: 1   the same call completes the handshake (SYN -> SYN-ACK -> ACK)
//   SND: 36  net_send() stages + queues the fixed 36-byte HTTP request
//   RCV: 36  net_stack_poll() advances the stack; response body received
//
// Wire shape (docs/phase2d-wire.md, single source of truth):
//   request : POST http://10.0.2.2:8080/completion, body
//             {"tool":"frame_tick","args":[0,1,2]} (36 bytes, no trailing NL)
//   response: HTTP 200, JSON body <= 256 bytes (net_response_len/byte window)
//
// Stack layout (all static, no malloc):
//   TX frame  : 1 x 512 B staging (Ethernet 14 + IPv4 20 + TCP 20 + payload)
//   RX parse  : reads the 2d-1 held RX frame via net_rx_byte() (no copy)
//   response  : 1 x 256 B static body buffer (wire-shape §3.2 limit)
//
// Fuel discipline: every RX wait goes through 2d-1's net_rx_wait(), which is a
// bounded poll (yields to the QEMU main loop every 256 iterations and never
// hangs). net_stack_poll() additionally fails closed after a bounded number of
// polls (NS_FAILED), so the Curlee-side `while (net_stack_poll() == 0)` loop
// always terminates — mirroring the kernel's fuel-bounded while loops.
//
// PVH sizing (LOCKED in the 2d-2 spec): on the PVH build (JOE_PVH_BOOT) the
// static buffers compile to 1 byte and every extern returns 0 — the API
// surface is identical on both paths (mirrors fb.c / virtio_net.c).
//
// Design laws honored:
//   - Single address space, Ring 0: raw Ethernet frames, trusted access gated
//     by this driver being the only toucher (mirrors Phys<T> discipline).
//   - Deterministic: fixed IPs (10.0.2.15/24, gw 10.0.2.2), fixed request,
//     fixed response envelope, bounded polls.
//   - Minimal footprint: no libc, no malloc, no String/Vec; static buffers
//     only.

#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "net_stack.h"

// ---------------------------------------------------------------------------
// Fixed network constants (LOCKED by QEMU user-net + the wire doc)
// ---------------------------------------------------------------------------
#define GW_IP_A 10
#define GW_IP_B 0
#define GW_IP_C 2
#define GW_IP_D 2            // gateway 10.0.2.2
#define OUR_IP_A 10
#define OUR_IP_B 0
#define OUR_IP_C 2
#define OUR_IP_D 15          // guest 10.0.2.15 (QEMU user-net)

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV4 0x0800

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

#define IP_VERSION_IHL 0x45    // v4, 5 words (20-byte header)
#define IP_TTL 64
#define IP_PROTO_TCP 6

#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_PSH 0x08

// Fixed host port (wire doc: llama.cpp / stub server on host :8080, reachable
// at the slirp gateway alias 10.0.2.2:8080).
#define HOST_PORT 8080

// Slirp user-net virtual gateway MAC (LOCKED for the 2d-4 smoke path).
//
// QEMU's built-in slirp (user-net) gives the guest a virtual gateway at
// 10.0.2.2 whose MAC is a FIXED constant: 52:55:0a:00:02:02 — the slirp
// "special" MAC prefix 52:55 followed by the gateway IPv4 address (10.0.2.2).
// This is how slirp presents its virtual devices on the wire (the gateway at
// 10.0.2.2, DNS at 10.0.2.3, etc. each get 52:55:<ip>). Empirically on
// QEMU 10.0.11 / libslirp 4.8 (the documented gap, kernel/virtio_net.c:
// "the plan's slirp auto-ARP-answer does not fire on QEMU 10.0.11 user-net"),
// slirp does NOT answer a bare guest ARP request for 10.0.2.2 — but the
// special MAC is a fixed implementation constant, so the stack resolves the
// gateway by CONSTANT instead of an ARP exchange.
//
// This preserves the LOCKED routing (no hostfwd; guest reaches the host stub
// at the 10.0.2.2 gateway alias) and keeps the round-trip deterministic. The
// guest IP (10.0.2.15) and the gateway IP (10.0.2.2) remain the wire-doc
// values.
#define SLIRP_GW_MAC_B0 0x52
#define SLIRP_GW_MAC_B1 0x55
#define SLIRP_GW_MAC_B2 0x0A
#define SLIRP_GW_MAC_B3 0x00
#define SLIRP_GW_MAC_B4 0x02
#define SLIRP_GW_MAC_B5 0x02

// ---------------------------------------------------------------------------
// Static state + buffers (no malloc anywhere)
// ---------------------------------------------------------------------------
#ifndef JOE_PVH_BOOT
// Bounded poll budget for net_stack_poll() — each poll does real RX work via
// net_rx_wait(), so this only bounds pathological non-TCP frame floods.
#define NET_STACK_POLL_MAX 1000
// TX staging frame: Ethernet(14) + IPv4(20) + TCP(20) + payload(153) + slack.
#define NET_STACK_TX_FRAME 512
// Response body buffer (wire-shape §3.2: <= 256 B).
#define NET_STACK_RESP_BODY 256

static unsigned char ns_tx_frame[NET_STACK_TX_FRAME];
static unsigned char ns_resp_body[NET_STACK_RESP_BODY];
static unsigned char ns_hdr_buf[NET_STACK_RESP_BODY];  // HTTP header scan
#else
#define NET_STACK_POLL_MAX 0
#define NET_STACK_TX_FRAME 1
#define NET_STACK_RESP_BODY 1
static unsigned char ns_tx_frame[1];
static unsigned char ns_resp_body[1];
static unsigned char ns_hdr_buf[1];
#endif

// Stack state machine (one-shot; only NS_RESP_READY / NS_FAILED are terminal).
enum ns_state {
    NS_IDLE = 0,        // not connected / nothing in flight
    NS_ARP_SENT,        // ARP request queued; waiting for the reply
    NS_ARP_RESOLVED,    // gateway MAC cached
    NS_TCP_CONNECTING,  // SYN sent; waiting for SYN-ACK
    NS_TCP_ESTABLISHED, // handshake done; ready for net_send
    NS_TCP_SENT,        // request queued; waiting for the response
    NS_RESP_HEADER,     // parsing HTTP response headers
    NS_RESP_BODY,       // copying the JSON body
    NS_RESP_READY,      // response body complete; net_response_len() valid
    NS_FAILED           // fuel exhausted or protocol error
};

static int ns_state = NS_IDLE;
static unsigned char ns_gw_mac[6];
static unsigned char ns_gw_mac_known = 0;

// TCP connection state (one fixed slot).
static unsigned int ns_my_seq = 0;
static unsigned int ns_peer_seq = 0;
static unsigned short ns_src_port = 0;   // our ephemeral port (fixed)
static unsigned short ns_dst_port = 0;

// HTTP response parse state (streamed across polls).
// NOTE (FIX 3, 2d-2 review): ONLY Content-Length-framed responses are
// supported — chunked transfer-encoding is intentionally NOT decoded. The
// host stub (2d-4) always sends Content-Length; a chunked response would
// overrun the body copy and hit the 256-byte truncation path (result 3).
static int ns_resp_len = 0;      // bytes copied into ns_resp_body so far
static int ns_hdr_done = 0;      // 1 = blank line seen, body phase active
static int ns_hdr_len = 0;       // header bytes buffered in ns_hdr_buf
static int ns_hdr_line_had_content = 0;  // current header line has bytes
static int ns_hdr_prev_cr = 0;   // saw '\r' (expecting '\n')
static int ns_content_length = -1;       // bytes expected in the body
static int ns_resp_truncated = 0;        // 1 = Content-Length > 256 cap

static long long ns_polls = 0;   // net_stack_poll() fuel counter

// ---------------------------------------------------------------------------
// Freestanding helpers (no libc)
// ---------------------------------------------------------------------------
static unsigned short ns_rd16(const unsigned char* p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static void ns_wr16(unsigned char* p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

static void ns_wr32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

static void ns_zero(void* p, unsigned long n)
{
    unsigned char* d = (unsigned char*)p;
    while (n > 0)
    {
        *d++ = 0;
        --n;
    }
}

static int ns_mac_zero(const unsigned char* m)
{
    return m[0] == 0 && m[1] == 0 && m[2] == 0 &&
           m[3] == 0 && m[4] == 0 && m[5] == 0;
}

static void ns_copy_mac(unsigned char* dst, const unsigned char* src)
{
    int i;
    for (i = 0; i < 6; ++i)
    {
        dst[i] = src[i];
    }
}

// Internet checksum (RFC 1071): ones-complement sum of 16-bit big-endian
// words, folded to 16 bits. `len` may be odd (last byte zero-padded).
static unsigned short ns_checksum(const unsigned char* data, unsigned int len)
{
    unsigned int sum = 0;
    unsigned int i;
    for (i = 0; i + 1 < len; i += 2)
    {
        sum += ns_rd16(data + i);
        if (sum > 0xFFFFu)
        {
            sum = (sum & 0xFFFFu) + (sum >> 16);  // end-around carry
        }
    }
    if (len & 1u)
    {
        sum += (unsigned int)data[len - 1] << 8;  // zero-padded high byte
        if (sum > 0xFFFFu)
        {
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
    }
    while (sum >> 16)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

// Read a 16-bit big-endian value from the held RX frame at byte offset `off`.
static unsigned short ns_rd16_byte(long long off)
{
    return (unsigned short)(((unsigned short)(unsigned char)net_rx_byte(off) << 8) |
                            (unsigned short)(unsigned char)net_rx_byte(off + 1));
}

// Read a 32-bit big-endian value from the held RX frame at byte offset `off`.
static unsigned int ns_rd32_byte(long long off)
{
    return ((unsigned int)(unsigned char)net_rx_byte(off) << 24) |
           ((unsigned int)(unsigned char)net_rx_byte(off + 1) << 16) |
           ((unsigned int)(unsigned char)net_rx_byte(off + 2) << 8) |
           (unsigned int)(unsigned char)net_rx_byte(off + 3);
}

// ---------------------------------------------------------------------------
// TX helpers: build + queue an Ethernet frame via the 2d-1 TX staging API.
// ---------------------------------------------------------------------------

// Begin a frame: Ethernet II header (dst MAC, src = our MAC from 2d-1,
// ethertype). Returns 1 on success.
static int ns_tx_begin(const unsigned char* dst, unsigned short ethertype)
{
    long long i;
    if (net_tx_stage_byte(12, (long long)(ethertype >> 8)) != 1 ||
        net_tx_stage_byte(13, (long long)(ethertype & 0xFF)) != 1)
    {
        return 0;
    }
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(i, (long long)dst[(unsigned int)i]);
        net_tx_stage_byte(6 + i, net_mac_byte(i));
    }
    return 1;
}

// Queue the staged frame of `len` bytes on the wire.
static int ns_tx_send(int len)
{
    return (int)net_tx_send((long long)len);
}

// ---------------------------------------------------------------------------
// ARP (RFC 826): resolve gateway 10.0.2.2 -> MAC.
// ---------------------------------------------------------------------------
static const unsigned char ns_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Send the ARP request for the gateway (broadcast dst). Returns net_tx_send.
static int ns_arp_request(void)
{
    long long i;
    if (!ns_tx_begin(ns_broadcast_mac, ETHERTYPE_ARP))
    {
        return 0;
    }
    // ARP header (28 bytes at offset 14).
    net_tx_stage_byte(14, 0x00);  // htype = Ethernet
    net_tx_stage_byte(15, 0x01);
    net_tx_stage_byte(16, 0x08);  // ptype = IPv4
    net_tx_stage_byte(17, 0x00);
    net_tx_stage_byte(18, 6);     // hlen
    net_tx_stage_byte(19, 4);     // plen
    net_tx_stage_byte(20, 0x00);  // op = request
    net_tx_stage_byte(21, ARP_OP_REQUEST);
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(22 + i, net_mac_byte(i));  // sha
    }
    net_tx_stage_byte(28, OUR_IP_A);  // spa
    net_tx_stage_byte(29, OUR_IP_B);
    net_tx_stage_byte(30, OUR_IP_C);
    net_tx_stage_byte(31, OUR_IP_D);
    for (i = 0; i < 6; ++i)
    {
        net_tx_stage_byte(32 + i, 0);  // tha
    }
    net_tx_stage_byte(38, GW_IP_A);  // tpa = 10.0.2.2
    net_tx_stage_byte(39, GW_IP_B);
    net_tx_stage_byte(40, GW_IP_C);
    net_tx_stage_byte(41, GW_IP_D);
    // Pad to the 60-byte Ethernet minimum (42 -> 60), like 2d-1.
    {
        long long p;
        for (p = 42; p < 60; ++p)
        {
            net_tx_stage_byte(p, 0);
        }
    }
    return ns_tx_send(60);
}

// Parse the held RX frame as an ARP reply for our IP; on success cache the
// sender MAC (the gateway's). Returns 1 when the gateway MAC was learned.
static int ns_arp_parse(void)
{
    long long len = net_rx_len();
    long long i;
    if (len < 42)
    {
        return 0;
    }
    // Ethernet II: ethertype must be ARP.
    if (net_rx_byte(12) != (ETHERTYPE_ARP >> 8) ||
        net_rx_byte(13) != (ETHERTYPE_ARP & 0xFF))
    {
        return 0;
    }
    // ARP header: op must be reply.
    if (net_rx_byte(20) != 0 || net_rx_byte(21) != ARP_OP_REPLY)
    {
        return 0;
    }
    // Sender protocol address must be the gateway 10.0.2.2.
    if (net_rx_byte(28) != GW_IP_A || net_rx_byte(29) != GW_IP_B ||
        net_rx_byte(30) != GW_IP_C || net_rx_byte(31) != GW_IP_D)
    {
        return 0;
    }
    // Target protocol address must be us.
    if (net_rx_byte(38) != OUR_IP_A || net_rx_byte(39) != OUR_IP_B ||
        net_rx_byte(40) != OUR_IP_C || net_rx_byte(41) != OUR_IP_D)
    {
        return 0;
    }
    for (i = 0; i < 6; ++i)
    {
        ns_gw_mac[(unsigned int)i] = (unsigned char)net_rx_byte(22 + i);  // sha
    }
    if (ns_mac_zero(ns_gw_mac))
    {
        return 0;
    }
    ns_gw_mac_known = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// IPv4 (RFC 791) + TCP (RFC 793) segment construction.
// ---------------------------------------------------------------------------

// Build the 20-byte IPv4 header at frame offset 14 for a TCP segment of
// `tcp_len` bytes (TCP header + payload). Returns the header checksum.
static unsigned short ns_ip_build(unsigned char* ip, unsigned int tcp_len)
{
    const unsigned int total = 20 + tcp_len;
    ip[0] = IP_VERSION_IHL;
    ip[1] = 0;                       // DSCP/ECN
    ns_wr16(ip + 2, (unsigned short)total);
    ns_wr16(ip + 4, 0x1234);         // identification (fixed, one-shot)
    ns_wr16(ip + 6, 0x4000);         // flags DF, frag offset 0
    ip[8] = IP_TTL;
    ip[9] = IP_PROTO_TCP;
    ns_wr16(ip + 10, 0);             // checksum filled below
    ip[12] = OUR_IP_A;               // src
    ip[13] = OUR_IP_B;
    ip[14] = OUR_IP_C;
    ip[15] = OUR_IP_D;
    ip[16] = GW_IP_A;                // dst
    ip[17] = GW_IP_B;
    ip[18] = GW_IP_C;
    ip[19] = GW_IP_D;
    return ns_checksum(ip, 20);
}

// TCP checksum (RFC 793): pseudo-header (src IP, dst IP, 0, proto, TCP len) +
// the TCP segment bytes at ns_tx_frame + 34 (checksum field still 0).
static unsigned short ns_tcp_checksum(const unsigned char* ip, unsigned int tcp_len)
{
    unsigned char ph[12];
    unsigned int sum = 0;
    unsigned int i;
    ns_copy_mac(ph, ip + 12);        // src IP
    ns_copy_mac(ph + 4, ip + 16);    // dst IP
    ph[8] = 0;
    ph[9] = IP_PROTO_TCP;
    ns_wr16(ph + 10, (unsigned short)tcp_len);
    for (i = 0; i < 12; i += 2)
    {
        sum += ns_rd16(ph + i);
        if (sum > 0xFFFFu)
        {
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
    }
    for (i = 0; i + 1 < tcp_len; i += 2)
    {
        sum += ns_rd16(ns_tx_frame + 34 + i);
        if (sum > 0xFFFFu)
        {
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
    }
    if (tcp_len & 1u)
    {
        sum += (unsigned int)ns_tx_frame[34 + tcp_len - 1] << 8;  // zero-pad
        if (sum > 0xFFFFu)
        {
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
    }
    while (sum >> 16)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

// Build + queue a TCP segment. `payload` is the byte source (0 = none) and
// `payload_len` its length; `flags` the TCP control bits; `seq`/`ack` the
// segment's sequence numbers. Returns net_tx_send().
static int ns_tcp_send_segment(const unsigned char* payload, int payload_len,
                               unsigned int flags, unsigned int seq,
                               unsigned int ack)
{
    unsigned char* ip = ns_tx_frame + 14;
    unsigned char* tcp = ns_tx_frame + 34;
    const int tcp_len = 20 + payload_len;
    const int frame_len = 14 + 20 + tcp_len;
    int send_len = frame_len;
    int i;

    // Ethernet II header FIRST (dst = gateway MAC, src = our MAC, ethertype
    // IPv4). CRITICAL (2d-4 finding): the SYN/ACK segments must carry a real
    // Ethernet header or slirp drops them (no SYN-ACK ever comes back).
    //
    // NOTE: unlike ns_arp_request (which stages directly into the driver TX
    // buffer via net_tx_stage_byte), this function builds the WHOLE frame in
    // the local ns_tx_frame[] and copies it out at the end. So the header
    // must be written into ns_tx_frame[0..13] here — calling ns_tx_begin()
    // would write to the driver buffer and then get OVERWRITTEN by the final
    // copy loop (the bug that sent zero-MAC SYNs).
    for (i = 0; i < 6; ++i)
    {
        ns_tx_frame[i] = ns_gw_mac[(unsigned int)i];       // dst = gateway
        ns_tx_frame[6 + i] = (unsigned char)net_mac_byte(i);  // src = our MAC
    }
    ns_tx_frame[12] = (unsigned char)(ETHERTYPE_IPV4 >> 8);
    ns_tx_frame[13] = (unsigned char)(ETHERTYPE_IPV4 & 0xFF);
    ns_zero(ip, 20);
    ns_zero(tcp, 20);
    ns_wr16(tcp + 0, ns_src_port);
    ns_wr16(tcp + 2, ns_dst_port);
    ns_wr32(tcp + 4, seq);
    ns_wr32(tcp + 8, ack);
    tcp[12] = (unsigned char)(5 << 4);   // data offset = 5 (20-byte header)
    tcp[13] = (unsigned char)flags;
    ns_wr16(tcp + 14, 0xFFFF);           // window (slirp accepts)
    ns_wr16(tcp + 16, 0);                // checksum filled below
    ns_wr16(tcp + 18, 0);                // urgent pointer
    if (payload_len > 0 && payload != 0)
    {
        for (i = 0; i < payload_len; ++i)
        {
            ns_tx_frame[34 + 20 + i] = payload[i];
        }
    }
    // IP header + checksum.
    ns_wr16(ip + 10, ns_ip_build(ip, (unsigned int)tcp_len));
    // TCP checksum over the pseudo-header + segment.
    ns_wr16(tcp + 16, ns_tcp_checksum(ip, (unsigned int)tcp_len));
    // Pad short frames to the 60-byte Ethernet minimum.
    if (send_len < 60)
    {
        for (i = send_len; i < 60; ++i)
        {
            ns_tx_frame[i] = 0;
        }
        send_len = 60;
    }
    // Queue via the 2d-1 TX API (one byte at a time, 0..send_len-1).
    for (i = 0; i < send_len; ++i)
    {
        net_tx_stage_byte((long long)i, (long long)ns_tx_frame[i]);
    }
    return ns_tx_send(send_len);
}

// ---------------------------------------------------------------------------
// RX: parse IPv4 + TCP segments delivered by 2d-1.
// ---------------------------------------------------------------------------

// Classify the held RX frame: 1 = it is an IPv4/TCP segment from the gateway
// to our IP/port. Fills:
//   *tcp_off    - offset of the TCP header in the frame
//   *ip_total   - the IPv4 total length field (header + TCP, from the IP
//                 header — NOT the Ethernet frame length)
//   *payload_len - TCP payload bytes (ip_total - ip_hl - tcp_hlen), i.e. the
//                 bytes to feed ns_http_byte(). Derived from the IP total
//                 length so Ethernet padding (frames padded to the 60-byte
//                 minimum) is NEVER mistaken for body bytes.
// Returns 0 otherwise.
static int ns_rx_tcp(long long len, int* tcp_off, int* ip_total_out,
                     int* payload_len_out)
{
    const unsigned short ip_total = ns_rd16_byte(14 + 2);
    const int ip_hl = (net_rx_byte(14) & 0x0F) * 4;
    int tcp_off0;
    int tcp_hlen;
    if (len < 54)
    {
        return 0;  // Ethernet(14) + IPv4(20) + TCP(20)
    }
    if (net_rx_byte(12) != (ETHERTYPE_IPV4 >> 8) ||
        net_rx_byte(13) != (ETHERTYPE_IPV4 & 0xFF))
    {
        return 0;
    }
    if (net_rx_byte(14 + 9) != IP_PROTO_TCP)
    {
        return 0;
    }
    // Destination IP must be us; source IP should be the gateway alias.
    if (net_rx_byte(14 + 16) != OUR_IP_A || net_rx_byte(14 + 17) != OUR_IP_B ||
        net_rx_byte(14 + 18) != OUR_IP_C || net_rx_byte(14 + 19) != OUR_IP_D)
    {
        return 0;
    }
    if (net_rx_byte(14 + 12) != GW_IP_A || net_rx_byte(14 + 13) != GW_IP_B ||
        net_rx_byte(14 + 14) != GW_IP_C || net_rx_byte(14 + 15) != GW_IP_D)
    {
        return 0;
    }
    // IP total length bounds the segment. The frame may be LONGER (Ethernet
    // padding), never shorter than the IP datagram.
    if (ip_total < 20 || ip_total > (unsigned short)(len - 14))
    {
        return 0;
    }
    tcp_off0 = 14 + ip_hl;
    if (tcp_off0 + 20 > len)
    {
        return 0;
    }
    // Source port should be the host's; destination port must be ours.
    if (ns_rd16_byte(tcp_off0 + 0) != ns_dst_port)
    {
        return 0;
    }
    if (ns_rd16_byte(tcp_off0 + 2) != ns_src_port)
    {
        return 0;
    }
    tcp_hlen = (net_rx_byte(tcp_off0 + 12) >> 4) * 4;
    if (tcp_off0 + tcp_hlen > len)
    {
        return 0;
    }
    // CRITICAL (2d-2 review BLOCKER): the payload length comes from the IP
    // total-length field, NOT from the frame length. The frame may carry
    // Ethernet padding up to the 60-byte minimum; using `len` here would feed
    // padding bytes into the HTTP body parser, corrupting ns_resp_body and
    // breaking the Content-Length match.
    *tcp_off = tcp_off0;
    *ip_total_out = (int)ip_total;
    *payload_len_out = (int)ip_total - ip_hl - tcp_hlen;
    if (*payload_len_out < 0)
    {
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// HTTP response parser (deterministic, bounded; wire-shape §3/§4)
// ---------------------------------------------------------------------------

// Parse Content-Length from the buffered header text (case-insensitive).
//
// IMPORTANT (FIX 3, 2d-2 review): the DECLARED value is preserved, NOT clamped
// to NET_STACK_RESP_BODY. The body copy is bounded by the 256-byte buffer, but
// ns_http_byte() uses the real Content-Length to decide "complete" vs
// "truncated": a declared length > 256 must surface as truncation (poll result
// 3), never as a misleading "ready" at 256 bytes. The digit loop saturates at
// 1,000,000 to avoid int overflow on a malicious header.
static void ns_http_content_length(void)
{
    int i;
    ns_content_length = -1;  // no Content-Length seen yet
    for (i = 0; i + 14 < ns_hdr_len; ++i)
    {
        if ((ns_hdr_buf[i] | 32) == 'c' && (ns_hdr_buf[i + 1] | 32) == 'o' &&
            (ns_hdr_buf[i + 2] | 32) == 'n' && (ns_hdr_buf[i + 3] | 32) == 't' &&
            (ns_hdr_buf[i + 4] | 32) == 'e' && (ns_hdr_buf[i + 5] | 32) == 'n' &&
            (ns_hdr_buf[i + 6] | 32) == 't' && ns_hdr_buf[i + 7] == '-' &&
            (ns_hdr_buf[i + 8] | 32) == 'l' && (ns_hdr_buf[i + 9] | 32) == 'e' &&
            (ns_hdr_buf[i + 10] | 32) == 'n' && (ns_hdr_buf[i + 11] | 32) == 'g' &&
            (ns_hdr_buf[i + 12] | 32) == 't' && (ns_hdr_buf[i + 13] | 32) == 'h' &&
            ns_hdr_buf[i + 14] == ':')
        {
            int v = 0;
            int j = i + 15;
            while (j < ns_hdr_len && ns_hdr_buf[j] == ' ')
            {
                ++j;
            }
            while (j < ns_hdr_len && ns_hdr_buf[j] >= '0' && ns_hdr_buf[j] <= '9')
            {
                v = v * 10 + (ns_hdr_buf[j] - '0');
                if (v > 1000000)
                {
                    v = 1000000;  // saturate: definitely over the 256 cap
                    break;
                }
                ++j;
            }
            if (v > 0)
            {
                ns_content_length = v;
            }
            break;
        }
    }
}

// Scan one byte of the HTTP response stream. Header phase: buffer bytes until
// the blank line, then parse Content-Length. Body phase: copy up to
// ns_content_length bytes into ns_resp_body.
// Returns:
//   0 = still reading
//   1 = body complete (Content-Length bytes received)
//   2 = truncated — the declared Content-Length exceeds the 256-byte response
//       cap (wire doc §3.2), so the body cannot be delivered intact. The
//       caller must treat this as a hard failure (poll result 3), never as
//       "ready".
static int ns_http_byte(unsigned char b)
{
    if (!ns_hdr_done)
    {
        // Header phase.
        if (ns_hdr_len < NET_STACK_RESP_BODY)
        {
            ns_hdr_buf[ns_hdr_len++] = b;
        }
        if (b == '\r')
        {
            ns_hdr_prev_cr = 1;
            return 0;
        }
        if (b == '\n' && ns_hdr_prev_cr)
        {
            ns_hdr_prev_cr = 0;
            if (!ns_hdr_line_had_content)
            {
                // Blank line -> headers done.
                ns_hdr_done = 1;
                ns_http_content_length();
                if (ns_content_length > NET_STACK_RESP_BODY)
                {
                    // FIX 3: the declared body is larger than the fixed
                    // response buffer — flag truncation immediately so the
                    // poll loop can return 3 instead of a misleading "ready"
                    // with a 256-byte prefix. (wire doc §4 error code 1:
                    // "response too long (> 256 bytes)".)
                    ns_resp_truncated = 1;
                    return 2;
                }
                return 0;
            }
            ns_hdr_line_had_content = 0;
            return 0;
        }
        if (b != '\n')
        {
            ns_hdr_line_had_content = 1;
        }
        return 0;
    }
    // Body phase: copy body bytes (bounded by Content-Length / 256 cap).
    if (ns_resp_len < NET_STACK_RESP_BODY)
    {
        ns_resp_body[ns_resp_len++] = b;
        if (ns_content_length >= 0 && ns_resp_len >= ns_content_length)
        {
            return 1;  // body complete
        }
    }
    else
    {
        // Reached the 256-byte cap without Content-Length satisfied (a
        // length-less or over-long response): fail closed.
        ns_resp_truncated = 1;
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Stack poll: wait for + process response frames; advance ONE step.
// Returns:
//   0 = still reading (call again)
//   1 = response ready (net_response_len()/net_response_byte() valid)
//   2 = fuel exhausted / protocol error (failed closed, terminal)
//   3 = response truncated — Content-Length exceeds the 256-byte cap
//       (wire doc §4 error code 1); ns_resp_len is invalidated (-1) and
//       net_response_len() reports 0 so the parser can never see a
//       misleading partial body.
// ---------------------------------------------------------------------------
long long net_stack_poll(void)
{
#ifdef JOE_PVH_BOOT
    // FIX 2 (2d-2 review): fail closed, never 0. The Curlee contract is
    // 0 = still reading, 1 = ready, 2 = fuel exhausted / failed, 3 =
    // truncated. On the PVH stub path the stack never runs, so report
    // "failed" — a future caller's `while (net_stack_poll() == 0)` loop
    // cannot spin forever on the stub.
    return 2;
#else
    if (ns_state == NS_RESP_READY)
    {
        return 1;
    }
    if (ns_state == NS_FAILED)
    {
        return 2;
    }
    if (ns_state != NS_TCP_SENT && ns_state != NS_RESP_HEADER &&
        ns_state != NS_RESP_BODY)
    {
        return 0;  // nothing to do yet (not connected / not sent)
    }
    if (++ns_polls > NET_STACK_POLL_MAX)
    {
        ns_state = NS_FAILED;
        return 2;
    }
    // Wait for the next frame (2d-1's fuel-bounded wait yields to QEMU).
    if (!net_rx_len())
    {
        if (net_rx_wait() == 0)
        {
            ns_state = NS_FAILED;
            return 2;
        }
    }
    // Process the held frame.
    {
        int tcp_off = 0;
        int ip_total = 0;
        int payload_len = 0;
        if (ns_rx_tcp(net_rx_len(), &tcp_off, &ip_total, &payload_len))
        {
            const int tcp_hlen = (net_rx_byte(tcp_off + 12) >> 4) * 4;
            int i;
            for (i = 0; i < payload_len; ++i)
            {
                const int r = ns_http_byte(
                    (unsigned char)net_rx_byte(tcp_off + tcp_hlen + i));
                if (r == 1)
                {
                    ns_state = NS_RESP_READY;
                    net_rx_done();
                    return 1;
                }
                if (r == 2)
                {
                    ns_state = NS_FAILED;  // truncated: Content-Length > 256
                    ns_resp_len = -1;      // invalidate the response
                    net_rx_done();
                    return 3;              // truncated / failed
                }
            }
            if (payload_len > 0)
            {
                ns_state = NS_RESP_BODY;  // keep parsing on the next poll
            }
        }
    }
    net_rx_done();
    if (ns_state == NS_RESP_READY)
    {
        return 1;
    }
    if (ns_state == NS_FAILED)
    {
        return 2;
    }
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Extern surface (Curlee window, docs/phase2d-wire.md §6)
// ---------------------------------------------------------------------------

// Connect to gw:port — ARP resolve + TCP handshake, fuel-bounded (via 2d-1's
// net_rx_wait). 1 = connected (NS_TCP_ESTABLISHED), 0 = failed / not ready.
//
// FIX 4 (2d-2 review): STRICTLY ONE-SHOT. The ephemeral port (49152), initial
// sequence (0x10000000) and IP identification are fixed constants chosen for a
// single fresh connection on a freshly-booted NIC. Re-connecting on the same
// boot is UNSUPPORTED: the fixed source port would collide with the host's
// TIME_WAIT state from the first connection, and the fixed sequence numbers
// would be rejected. Call once, send once, receive once.
long long net_connect(long long port)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (ns_state == NS_TCP_ESTABLISHED || ns_state == NS_TCP_SENT ||
        ns_state == NS_RESP_HEADER || ns_state == NS_RESP_BODY ||
        ns_state == NS_RESP_READY)
    {
        return 1;  // already connected / in flight (one-shot: no re-connect)
    }
    if (!net_link_up())
    {
        return 0;
    }
    // Fresh start: fixed ephemeral port + reset parse state.
    ns_src_port = 49152;                 // fixed, one-shot (fresh NIC, no clash)
    ns_dst_port = (unsigned short)port;
    ns_my_seq = 0x10000000;              // fixed initial sequence (one-shot)
    ns_peer_seq = 0;
    ns_resp_len = 0;
    ns_hdr_done = 0;
    ns_hdr_len = 0;
    ns_hdr_line_had_content = 0;
    ns_hdr_prev_cr = 0;
    ns_content_length = -1;
    ns_resp_truncated = 0;
    ns_polls = 0;
    // 1. ARP announce + gateway resolution.
    if (!ns_gw_mac_known)
    {
        // Send the gateway ARP request FIRST. Two reasons:
        //   (a) slirp LEARNS the guest's MAC/IP from the sender fields of any
        //       incoming ARP packet (arp_table_add) — without this, slirp
        //       cannot address its SYN-ACK back to the guest and silently
        //       drops the TCP handshake (empirically confirmed: a SYN with no
        //       prior ARP gets no reply; with the ARP first, it does);
        //   (b) a slirp that DOES answer gives us the real gateway MAC.
        // net_rx_wait() is fuel-bounded and now reclaims TX buffers, so it is
        // safe to run even though QEMU 10.0.11 slirp won't reply to a bare
        // guest ARP for 10.0.2.2 (the documented gap).
        if (ns_arp_request())
        {
            ns_state = NS_ARP_SENT;
            (void)net_rx_wait();   // bounded; gives slirp time to learn us
            if (ns_arp_parse())
            {
                net_rx_done();
            }
        }
        // Gateway MAC is then resolved by CONSTANT: slirp's virtual gateway
        // MAC (52:54:00:12:34:56) is fixed by the user-net implementation,
        // and we cannot rely on the ARP reply above (slirp 4.8 won't send
        // one for a bare request). The wire-doc "ARP: 1" marker (gateway
        // resolved) holds — the resolution is just by constant, not by
        // exchange, on the smoke path.
        ns_gw_mac[0] = SLIRP_GW_MAC_B0;
        ns_gw_mac[1] = SLIRP_GW_MAC_B1;
        ns_gw_mac[2] = SLIRP_GW_MAC_B2;
        ns_gw_mac[3] = SLIRP_GW_MAC_B3;
        ns_gw_mac[4] = SLIRP_GW_MAC_B4;
        ns_gw_mac[5] = SLIRP_GW_MAC_B5;
        ns_gw_mac_known = 1;
        ns_state = NS_ARP_RESOLVED;
    }
    // 2. TCP handshake: SYN -> SYN-ACK -> ACK.
    if (!ns_tcp_send_segment(0, 0, TCP_FLAG_SYN, ns_my_seq, 0))
    {
        ns_state = NS_FAILED;
        return 0;
    }
    ns_state = NS_TCP_CONNECTING;
    if (net_rx_wait() == 0)
    {
        ns_state = NS_FAILED;
        return 0;
    }
    {
        int tcp_off = 0;
        int ip_total = 0;
        int payload_len = 0;
        if (!ns_rx_tcp(net_rx_len(), &tcp_off, &ip_total, &payload_len))
        {
            ns_state = NS_FAILED;
            net_rx_done();
            return 0;
        }
        // FIX 4 (2d-2 review): this must be a SYN-ACK (both SYN and ACK set) —
        // slirp's SYN-ACK for our SYN carries both flags. A bare SYN (passive
        // open) or a bare ACK is not the reply we are waiting for.
        const unsigned int flags = (unsigned int)net_rx_byte(tcp_off + 13);
        if (!((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)))
        {
            ns_state = NS_FAILED;
            net_rx_done();
            return 0;
        }
        ns_peer_seq = ns_rd32_byte(tcp_off + 4);
    }
    net_rx_done();
    if (!ns_tcp_send_segment(0, 0, TCP_FLAG_ACK, ns_my_seq + 1, ns_peer_seq + 1))
    {
        ns_state = NS_FAILED;
        return 0;
    }
    ns_state = NS_TCP_ESTABLISHED;
    return 1;
#endif
}

// Send the fixed HTTP request (wire doc: 36-byte JSON body). The request is
// staged as a real HTTP POST with the exact body; 1 = queued.
//
// FIX 4 (2d-2 review): like net_connect(), this is strictly ONE-SHOT — it
// advances ns_my_seq by (1 + hlen + 36) and moves the state to NS_TCP_SENT,
// which is terminal for sending. Calling net_send() again on the same boot
// returns 0 (state is no longer NS_TCP_ESTABLISHED).
long long net_send(long long buf_len)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (ns_state != NS_TCP_ESTABLISHED)
    {
        return 0;
    }
    if (buf_len != 36)
    {
        return 0;  // wire doc: the request body is exactly 36 bytes
    }
    // The wire-doc request body (36 bytes, no trailing newline):
    //   {"tool":"frame_tick","args":[0,1,2]}
    static const unsigned char request[36] = {
        0x7B, 0x22, 0x74, 0x6F, 0x6F, 0x6C, 0x22, 0x3A,  // {"tool":
        0x22, 0x66, 0x72, 0x61, 0x6D, 0x65, 0x5F, 0x74,  // "frame_t
        0x69, 0x63, 0x6B, 0x22, 0x2C, 0x22, 0x61, 0x72,  // ick","ar
        0x67, 0x73, 0x22, 0x3A, 0x5B, 0x30, 0x2C, 0x31,  // gs":[0,1
        0x2C, 0x32, 0x5D, 0x7D                            // ,2]}
    };
    // Deterministic HTTP request (fixed-length framing, Connection: close so
    // the stub can match the bytes exactly):
    //   POST /completion HTTP/1.1\r\n
    //   Host: 10.0.2.2:8080\r\n
    //   Content-Type: application/json\r\n
    //   Content-Length: 36\r\n
    //   Connection: close\r\n
    //   \r\n
    //   <36-byte body>
    const unsigned char* h = (const unsigned char*)
        "POST /completion HTTP/1.1\r\n"
        "Host: 10.0.2.2:8080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 36\r\n"
        "Connection: close\r\n"
        "\r\n";
    unsigned char payload[256];
    int hlen = 0;
    int i;
    while (h[hlen] != 0)
    {
        payload[hlen] = h[hlen];
        ++hlen;
    }
    for (i = 0; i < 36; ++i)
    {
        payload[hlen + i] = request[i];
    }
    if (!ns_tcp_send_segment(payload, hlen + 36, TCP_FLAG_ACK | TCP_FLAG_PSH,
                             ns_my_seq + 1, ns_peer_seq + 1))
    {
        return 0;
    }
    ns_my_seq += (unsigned int)(1 + hlen + 36);
    ns_state = NS_TCP_SENT;
    return 1;
#endif
}

// Bytes of the HTTP response body (valid when net_stack_poll() returned 1).
// Returns 0 when no response is ready AND when the response was truncated
// (ns_resp_len is invalidated to -1 by the truncation path, so a truncated
// body never masquerades as a valid length).
long long net_response_len(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (ns_state == NS_RESP_READY) ? (long long)ns_resp_len : 0;
#endif
}

// Byte i of the response body (0..net_response_len()-1; 0 if out of range).
long long net_response_byte(long long i)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    if (ns_state != NS_RESP_READY || i < 0 || i >= (long long)ns_resp_len)
    {
        return 0;
    }
    return (long long)ns_resp_body[(unsigned int)i];
#endif
}

// C-side convenience accessor (not in the Curlee extern surface — 2d-3 reads
// bytes through net_response_byte): pointer to the response body, or 0 when
// no response is ready.
const unsigned char* net_response_ptr(void)
{
#ifdef JOE_PVH_BOOT
    return 0;
#else
    return (ns_state == NS_RESP_READY) ? ns_resp_body : 0;
#endif
}
