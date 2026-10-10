/* Instruction semantics shared by the interpreter and the generated (recompiled) code. */
#ifndef OPS_H
#define OPS_H
#include "psx.h"

static inline u32 op_lwl(u32 addr, u32 rt) {
    u32 w = rd32(addr & ~3u);
    switch (addr & 3) {
    case 0: return (rt & 0x00ffffffu) | (w << 24);
    case 1: return (rt & 0x0000ffffu) | (w << 16);
    case 2: return (rt & 0x000000ffu) | (w << 8);
    default: return w;
    }
}
static inline u32 op_lwr(u32 addr, u32 rt) {
    u32 w = rd32(addr & ~3u);
    switch (addr & 3) {
    case 0: return w;
    case 1: return (rt & 0xff000000u) | (w >> 8);
    case 2: return (rt & 0xffff0000u) | (w >> 16);
    default: return (rt & 0xffffff00u) | (w >> 24);
    }
}
static inline void op_swl(u32 addr, u32 rt) {
    u32 a = addr & ~3u, w = rd32(a);
    switch (addr & 3) {
    case 0: w = (w & 0xffffff00u) | (rt >> 24); break;
    case 1: w = (w & 0xffff0000u) | (rt >> 16); break;
    case 2: w = (w & 0xff000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    wr32(a, w);
}
static inline void op_swr(u32 addr, u32 rt) {
    u32 a = addr & ~3u, w = rd32(a);
    switch (addr & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000ffu) | (rt << 8); break;
    case 2: w = (w & 0x0000ffffu) | (rt << 16); break;
    default: w = (w & 0x00ffffffu) | (rt << 24); break;
    }
    wr32(a, w);
}

static inline void op_mult(u32 a, u32 b) {
    s64 p = (s64)(s32)a * (s64)(s32)b;
    cpu.lo = (u32)p;
    cpu.hi = (u32)((u64)p >> 32);
}
static inline void op_multu(u32 a, u32 b) {
    u64 p = (u64)a * (u64)b;
    cpu.lo = (u32)p;
    cpu.hi = (u32)(p >> 32);
}
static inline void op_div(u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) {
        cpu.hi = (u32)n;
        cpu.lo = n >= 0 ? 0xffffffffu : 1u;
    } else if (n == (s32)0x80000000 && d == -1) {
        cpu.hi = 0;
        cpu.lo = 0x80000000u;
    } else {
        cpu.lo = (u32)(n / d);
        cpu.hi = (u32)(n % d);
    }
}
static inline void op_divu(u32 a, u32 b) {
    if (b == 0) {
        cpu.hi = a;
        cpu.lo = 0xffffffffu;
    } else {
        cpu.lo = a / b;
        cpu.hi = a % b;
    }
}

static inline u32 op_sext8(u32 v) { return (u32)(s32)(s8)v; }
static inline u32 op_sext16(u32 v) { return (u32)(s32)(s16)v; }

#endif
