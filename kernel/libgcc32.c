// SPDX-License-Identifier: GPL-3.0
//
// libgcc32.c — minimal freestanding 64-bit arithmetic helpers for the 32-bit
// GRUB path (kernel-grub.elf).
//
// The Curlee codegen emits int64_t arithmetic (Int -> int64_t). Compiled with
// -m32 (i386), GCC lowers 64-bit multiply/divide/modulo to libgcc helper calls
// (__muldi3 / __divdi3 / ...) that are NOT part of our freestanding runtime
// (-nostdlib, no libgcc).
//
// This file provides exactly those helpers, implemented with 32-bit
// primitives only so the helpers themselves never emit another 64-bit libgcc
// call (no recursion). It is linked ONLY into kernel-grub.elf (the GRUB ISO
// path). The 64-bit PVH path (kernel.elf) has native int64_t arithmetic and
// does not need it.
//
// Semantics match libgcc's runtime ABI (GCC's documented helper contracts).
// 64-bit divide-by-zero returns 0 — defined behavior for this kernel (the
// verifier already prevents division by zero at the Curlee level; this is
// belt-and-braces for the freestanding C).

typedef unsigned int        u32;
typedef int                 s32;
typedef unsigned long long  u64;
typedef long long           s64;

// ---------------------------------------------------------------------------
// 64-bit multiply (low 64 bits of the product).
// ---------------------------------------------------------------------------
u64 __muldi3(u64 a, u64 b)
{
    u32 a0 = (u32)a;
    u32 a1 = (u32)(a >> 32);
    u32 b0 = (u32)b;
    u32 b1 = (u32)(b >> 32);

    // a0*b0 computed exactly via 16-bit halves (no 32-bit overflow):
    //   a0*b0 = a0l*b0l + (a0l*b0h + a0h*b0l) << 16 + (a0h*b0h) << 32
    u32 a0l = a0 & 0xFFFFu, a0h = a0 >> 16;
    u32 b0l = b0 & 0xFFFFu, b0h = b0 >> 16;
    u64 t = (u64)(a0l * b0l)
          + (((u64)(a0l * b0h) + (u64)(a0h * b0l)) << 16)
          + ((u64)(a0h * b0h) << 32);

    // Cross terms contribute only their low 32 bits (mod 2^64).
    u32 cross = a0 * b1 + a1 * b0;

    return t + ((u64)cross << 32);
}

// ---------------------------------------------------------------------------
// Unsigned 64/64 division (+ optional remainder).
// ---------------------------------------------------------------------------
static u64 udivmoddi4_impl(u64 n, u64 d, u64* rp)
{
    u32 n0 = (u32)n, n1 = (u32)(n >> 32);
    u32 d0 = (u32)d, d1 = (u32)(d >> 32);

    // Divide by zero: defined (0) for this kernel.
    if (d1 == 0 && d0 == 0)
    {
        if (rp)
        {
            *rp = n;
        }
        return 0;
    }

    if (d1 == 0)
    {
        // Single-word divisor: one divl (or two); remainder < 2^32.
        if (n1 < d0)
        {
            u32 q, r;
            __asm__ volatile("divl %4" : "=a"(q), "=d"(r) : "a"(n0), "d"(n1), "r"(d0));
            if (rp)
            {
                *rp = (u64)r;
            }
            return (u64)q;
        }
        else
        {
            u32 q1, r1, q0, r0;
            __asm__ volatile("divl %4" : "=a"(q1), "=d"(r1) : "a"(0), "d"(n1), "r"(d0));
            __asm__ volatile("divl %4" : "=a"(q0), "=d"(r0) : "a"(n0), "d"(r1), "r"(d0));
            if (rp)
            {
                *rp = (u64)r0;
            }
            return ((u64)q1 << 32) | q0;
        }
    }

    // General case: 65-bit restoring long division, 32-bit ops only.
    {
        u32 r_hi = 0, r_lo = 0; // 64-bit remainder accumulator
        u32 r_ovf = 0;          // bit 64 of the accumulator
        u32 q_hi = 0, q_lo = 0;
        int i;
        for (i = 63; i >= 0; --i)
        {
            u32 bit = (i >= 32) ? ((n1 >> (i - 32)) & 1u) : ((n0 >> i) & 1u);
            u32 new_ovf = r_hi >> 31;
            r_hi = (r_hi << 1) | (r_lo >> 31);
            r_lo = (r_lo << 1) | bit;
            r_ovf = new_ovf;
            {
                int ge = r_ovf; // r >= 2^64 >= d -> definitely greater
                if (!ge)
                {
                    if (r_hi > d1)
                    {
                        ge = 1;
                    }
                    else if (r_hi < d1)
                    {
                        ge = 0;
                    }
                    else
                    {
                        ge = (r_lo >= d0);
                    }
                }
                if (ge)
                {
                    u32 borrow = (r_lo < d0) ? 1u : 0u;
                    r_lo -= d0;
                    r_hi = r_hi - d1 - borrow; // exact mod 2^64 (r_ovf covers the top)
                    r_ovf = 0;
                    if (i >= 32)
                    {
                        q_hi |= (1u << (i - 32));
                    }
                    else
                    {
                        q_lo |= (1u << i);
                    }
                }
            }
        }
        if (rp)
        {
            *rp = ((u64)r_hi << 32) | r_lo;
        }
        return ((u64)q_hi << 32) | q_lo;
    }
}

u64 __udivdi3(u64 a, u64 b)
{
    return udivmoddi4_impl(a, b, 0);
}

u64 __umoddi3(u64 a, u64 b)
{
    u64 r;
    udivmoddi4_impl(a, b, &r);
    return r;
}

u64 __udivmoddi4(u64 a, u64 b, u64* rp)
{
    return udivmoddi4_impl(a, b, rp);
}

// ---------------------------------------------------------------------------
// Signed wrappers (sign-magnitude).
// ---------------------------------------------------------------------------
static u64 neg_u64(u64 x)
{
    return 0 - x;
}

s64 __divdi3(s64 a, s64 b)
{
    int neg = 0;
    u64 ua, ub, q;
    if (a < 0)
    {
        ua = neg_u64((u64)a);
        neg ^= 1;
    }
    else
    {
        ua = (u64)a;
    }
    if (b < 0)
    {
        ub = neg_u64((u64)b);
        neg ^= 1;
    }
    else
    {
        ub = (u64)b;
    }
    q = udivmoddi4_impl(ua, ub, 0);
    return neg ? (s64)neg_u64(q) : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
    int neg = 0;
    u64 ua, ub, r;
    if (a < 0)
    {
        ua = neg_u64((u64)a);
        neg ^= 1;
    }
    else
    {
        ua = (u64)a;
    }
    if (b < 0)
    {
        ub = neg_u64((u64)b);
    }
    else
    {
        ub = (u64)b;
    }
    udivmoddi4_impl(ua, ub, &r);
    return neg ? (s64)neg_u64(r) : (s64)r;
}

s64 __negdi2(s64 x)
{
    return (s64)neg_u64((u64)x);
}

// ---------------------------------------------------------------------------
// Variable-count 64-bit shifts (emitted when code shifts a 64-bit value by a
// runtime amount; not used by the current Curlee codegen, provided for the C
// drivers and future code).
// ---------------------------------------------------------------------------
u64 __ashldi3(u64 x, int c)
{
    u32 lo = (u32)x, hi = (u32)(x >> 32);
    if (c >= 64)
    {
        return 0;
    }
    if (c >= 32)
    {
        hi = lo << (c - 32);
        lo = 0;
        return ((u64)hi << 32) | lo;
    }
    if (c == 0)
    {
        return x;
    }
    hi = (hi << c) | (lo >> (32 - c));
    lo = lo << c;
    return ((u64)hi << 32) | lo;
}

u64 __lshrdi3(u64 x, int c)
{
    u32 lo = (u32)x, hi = (u32)(x >> 32);
    if (c >= 64)
    {
        return 0;
    }
    if (c >= 32)
    {
        lo = hi >> (c - 32);
        hi = 0;
        return ((u64)hi << 32) | lo;
    }
    if (c == 0)
    {
        return x;
    }
    lo = (lo >> c) | (hi << (32 - c));
    hi = hi >> c;
    return ((u64)hi << 32) | lo;
}

s64 __ashrdi3(s64 x, int c)
{
    u32 lo = (u32)x, hi = (u32)((u64)x >> 32);
    u32 sign = hi >> 31;
    if (c >= 64)
    {
        hi = (u32)(0u - sign);
        lo = hi;
        return (s64)(((u64)hi << 32) | lo);
    }
    if (c >= 32)
    {
        lo = (u32)((s32)hi >> (c - 32));
        hi = (u32)(0u - sign);
        return (s64)(((u64)hi << 32) | lo);
    }
    if (c == 0)
    {
        return x;
    }
    lo = (lo >> c) | (hi << (32 - c));
    hi = (u32)((s32)hi >> c);
    return (s64)(((u64)hi << 32) | lo);
}

// ---------------------------------------------------------------------------
// 64-bit comparison (some GCC versions emit this for 64-bit compares).
// Returns >0, 0, <0 like memcmp.
// ---------------------------------------------------------------------------
s64 __cmpdi2(s64 a, s64 b)
{
    u32 a0 = (u32)a, a1 = (u32)((u64)a >> 32);
    u32 b0 = (u32)b, b1 = (u32)((u64)b >> 32);
    if (a1 != b1)
    {
        return ((s32)a1 > (s32)b1) ? 1 : -1;
    }
    if (a0 != b0)
    {
        return (a0 > b0) ? 1 : -1;
    }
    return 0;
}
