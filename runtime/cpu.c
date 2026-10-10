/* MIPS R3000A interpreter: the reference execution backend.
 * Also used as the fallback for any code the recompiler did not cover. */
#include "psx.h"
#include "ops.h"
#include <stdlib.h>

bool g_trace_exec = false;
u32 *g_pc_hist;
/* dynamic call/jump targets seen at run time (function pointers, vtables, jump tables): seeds for the recompiler */
static u8 *g_entry_bits;                /* bitmap over RAM words */
bool g_trace_entries;
static inline void note_entry(u32 a) { if (g_entry_bits) { u32 w = (a & 0x1ffffc) >> 2; g_entry_bits[w >> 3] |= (u8)(1u << (w & 7)); } }
void cpu_trace_entries_start(void) { g_entry_bits = calloc(RAM_SIZE / 4 / 8 + 1, 1); }
void cpu_trace_entries_dump(const char *path) {
    if (!g_entry_bits) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    for (u32 w = 0; w < RAM_SIZE / 4; w++) if (g_entry_bits[w >> 3] & (1u << (w & 7))) fprintf(f, "%08x\n", 0x80000000u + w * 4);
    fclose(f);
}
u32 g_break_pc[4]; int g_nbreak;
u32 g_watch_lo, g_watch_hi; int g_watch_n;
u32 g_wwatch_lo, g_wwatch_hi;       /* debug: log PCs writing to [lo,hi) (interpreter only) */       /* debug: log PCs reading from [lo,hi) */                 /* debug: print registers when these PCs execute */                         /* optional per-word execution counters (RAM) */
bool g_trace_io_all = false;
u64 g_interp_insns = 0;

#define GUEST_STOP 0x1ffffff0u   /* sentinel "return address" for guest_call */

static inline u32 bios_vec_of(u32 pc) {
    u32 p = pc & 0x1fffffffu;
    return (p == 0xa0 || p == 0xb0 || p == 0xc0) ? p : 0;
}

u32 rt_cop0_read(int reg) {
    switch (reg) {
    case 12: return cpu.cop0[12];
    case 13: {
        u32 c = cpu.cop0[13] & ~(1u << 10);
        if (irq_pending()) c |= 1u << 10;
        return c;
    }
    case 14: return cpu.cop0[14];
    case 15: return 0x00000002u; /* PRId */
    default: return cpu.cop0[reg & 31];
    }
}
void rt_cop0_write(int reg, u32 v) {
    switch (reg) {
    case 12: cpu.cop0[12] = v; break;
    case 13: cpu.cop0[13] = (cpu.cop0[13] & ~0x300u) | (v & 0x300u); break;
    default: cpu.cop0[reg & 31] = v; break;
    }
}
void rt_rfe(void) {
    u32 sr = cpu.cop0[12];
    cpu.cop0[12] = (sr & ~0xfu) | ((sr >> 2) & 0xfu);
}

void rt_break(u32 pc) {
    WARN("BREAK executed at %08x (code %05x)\n", pc, (rd32(pc) >> 6) & 0xfffff);
}

void vtrace_record(u32 pc, int preg, u32 pval);
extern bool g_vtrace;

void interp_run(u32 pc, u32 stop_pc) {
    u32 npc = pc + 4;
    u32 pend_reg = 0, pend_val = 0;
    u64 *cycles = &cpu.cycles;
    const bool sync = g_sync_mode;       /* look at hardware events only where recompiled code does */
    const bool use_recomp = g_cur_overlay && !g_force_interp;
    bool dslot = false;                  /* the instruction at pc sits in a branch delay slot */
    u32 bw_at = 1;                       /* target of a just-taken backward branch (sync-mode safe point) */

    for (;;) {
        if (likely(!sync)) {
            if (unlikely(*cycles >= g_next_event)) {
                cpu.pc = pc;
                rt_event();
            }
        }
        if (unlikely(pc == stop_pc)) return;
        if (!dslot && (use_recomp || sync)) {
            RecompFn rf = recomp_entry(pc);
            if (rf && use_recomp) {
                /* a recompiled function starts here: run it (it makes its own safe-point check) */
                if (pend_reg) { cpu.r[pend_reg] = pend_val; pend_reg = 0; }
                rf();
                pc = cpu.r[31];
                npc = pc + 4;
                bw_at = 1;
                continue;
            }
            if (sync && (rf || pc == bw_at)) {
                bw_at = 1;
                cpu.pc = pc;
                if (unlikely(g_vtrace)) vtrace_record(pc, (int)pend_reg, pend_val);
                if (*cycles >= g_next_event) rt_event();
                if (unlikely(pc == stop_pc)) return;
            }
        }
        {
            u32 vec = bios_vec_of(pc);
            if (unlikely(vec)) {
                bios_call(vec, cpu.r[9]);
                /* emulate "jr ra" */
                pc = cpu.r[31];
                npc = pc + 4;
                pend_reg = 0;
                continue;
            }
        }

        u32 op = rd32(pc);
        if (g_trace_exec) fprintf(stderr, "%08x: %08x\n", pc, op);
        if (g_pc_hist) g_pc_hist[(pc & 0x1ffffc) >> 2]++;
        for (int bi = 0; bi < g_nbreak; bi++) if (pc == g_break_pc[bi]) {
            fprintf(stderr, "[break %08x] cyc=%llu v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x s0=%08x s1=%08x sp=%08x ra=%08x\n", pc, (unsigned long long)*cycles,
                    cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.r[8], cpu.r[16], cpu.r[17], cpu.r[29], cpu.r[31]);
        }
        (*cycles)++;
        g_interp_insns++;

        u32 cur_pc = pc;
        pc = npc;
        npc = pc + 4;

        u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
        u32 a = cpu.r[rs], b = cpu.r[rt];
        u32 imm = op & 0xffff;
        u32 simm = (u32)(s32)(s16)imm;

        /* apply the previous instruction's load now (operands above were read pre-load) */
        u32 lreg = pend_reg, lval = pend_val;
        pend_reg = 0;
        /* lwl/lwr see the in-flight value of the same register */
        u32 b_fwd = (lreg && lreg == rt) ? lval : b;
        if (lreg) cpu.r[lreg] = lval;

#define SETR(n, v) do { u32 _n = (n); if (_n) cpu.r[_n] = (v); } while (0)
#define LOAD(n, v) do { u32 _n = (n); u32 _v = (v); if (_n) { pend_reg = _n; pend_val = _v; if (lreg == _n) {} } } while (0)
#define BRANCH_TO(t) do { npc = (t); if (sync && (t) <= cur_pc) bw_at = (t); } while (0)
#define CALL_TO(t) do { npc = (t); } while (0)
#define WWATCH(addr) do { if (unlikely(g_wwatch_hi) && (addr) >= g_wwatch_lo && (addr) < g_wwatch_hi) fprintf(stderr, "[wwatch] write to %08x at pc %08x ra=%08x cyc=%llu\n", (addr), cur_pc, cpu.r[31], (unsigned long long)*cycles); } while (0)
#define WATCH(addr) do { if (unlikely(g_watch_hi) && (addr) >= g_watch_lo && (addr) < g_watch_hi && g_watch_n < 400) { g_watch_n++; fprintf(stderr, "[watch] read %08x at pc %08x\n", (addr), cur_pc); } } while (0)

        switch (op >> 26) {
        case 0x00: {
            switch (op & 63) {
            case 0x00: SETR(rd, b << ((op >> 6) & 31)); break;
            case 0x02: SETR(rd, b >> ((op >> 6) & 31)); break;
            case 0x03: SETR(rd, (u32)((s32)b >> ((op >> 6) & 31))); break;
            case 0x04: SETR(rd, b << (a & 31)); break;
            case 0x06: SETR(rd, b >> (a & 31)); break;
            case 0x07: SETR(rd, (u32)((s32)b >> (a & 31))); break;
            case 0x08: if (rs != 31 && !bios_vec_of(a)) { note_entry(a); BRANCH_TO(a); } else CALL_TO(a); break;
            case 0x09: note_entry(a); SETR(rd, cur_pc + 8); CALL_TO(a); break;
            case 0x0c:
                if (lreg) { /* syscall may inspect registers: already applied */ }
                kernel_syscall(op);
                break;
            case 0x0d: rt_break(cur_pc); break;
            case 0x10: SETR(rd, cpu.hi); break;
            case 0x11: cpu.hi = a; break;
            case 0x12: SETR(rd, cpu.lo); break;
            case 0x13: cpu.lo = a; break;
            case 0x18: *cycles += 5; op_mult(a, b); break;
            case 0x19: *cycles += 5; op_multu(a, b); break;
            case 0x1a: *cycles += 30; op_div(a, b); break;
            case 0x1b: *cycles += 30; op_divu(a, b); break;
            case 0x20: case 0x21: SETR(rd, a + b); break;
            case 0x22: case 0x23: SETR(rd, a - b); break;
            case 0x24: SETR(rd, a & b); break;
            case 0x25: SETR(rd, a | b); break;
            case 0x26: SETR(rd, a ^ b); break;
            case 0x27: SETR(rd, ~(a | b)); break;
            case 0x2a: SETR(rd, (s32)a < (s32)b); break;
            case 0x2b: SETR(rd, a < b); break;
            default: WARN("illegal special op %08x at %08x\n", op, cur_pc); break;
            }
            break;
        }
        case 0x01: {
            bool ge = (op >> 16) & 1;
            bool link = ((op >> 17) & 0xf) == 8;
            bool cond = ge ? ((s32)a >= 0) : ((s32)a < 0);
            if (link) SETR(31, cur_pc + 8);
            if (cond) { if (link) CALL_TO(cur_pc + 4 + (simm << 2)); else BRANCH_TO(cur_pc + 4 + (simm << 2)); }
            break;
        }
        case 0x02: BRANCH_TO((cur_pc & 0xf0000000u) | ((op & 0x3ffffffu) << 2)); break;
        case 0x03: SETR(31, cur_pc + 8); note_entry((cur_pc & 0xf0000000u) | ((op & 0x3ffffffu) << 2)); CALL_TO((cur_pc & 0xf0000000u) | ((op & 0x3ffffffu) << 2)); break;
        case 0x04: if (a == b) BRANCH_TO(cur_pc + 4 + (simm << 2)); break;
        case 0x05: if (a != b) BRANCH_TO(cur_pc + 4 + (simm << 2)); break;
        case 0x06: if ((s32)a <= 0) BRANCH_TO(cur_pc + 4 + (simm << 2)); break;
        case 0x07: if ((s32)a > 0) BRANCH_TO(cur_pc + 4 + (simm << 2)); break;
        case 0x08: case 0x09: SETR(rt, a + simm); break;
        case 0x0a: SETR(rt, (s32)a < (s32)simm); break;
        case 0x0b: SETR(rt, a < simm); break;
        case 0x0c: SETR(rt, a & imm); break;
        case 0x0d: SETR(rt, a | imm); break;
        case 0x0e: SETR(rt, a ^ imm); break;
        case 0x0f: SETR(rt, imm << 16); break;
        case 0x10: { /* COP0 */
            u32 sub = rs;
            if (sub == 0) LOAD(rt, rt_cop0_read(rd));
            else if (sub == 4) rt_cop0_write(rd, b);
            else if (sub == 16 && (op & 63) == 16) rt_rfe();
            else WARN("cop0 op %08x at %08x\n", op, cur_pc);
            break;
        }
        case 0x12: { /* COP2 / GTE */
            if (op & (1u << 25)) gte_cmd(op & 0x1ffffffu);
            else switch (rs) {
            case 0: LOAD(rt, gte_mfc2(rd)); break;
            case 2: LOAD(rt, gte_cfc2(rd)); break;
            case 4: gte_mtc2(rd, b); break;
            case 6: gte_ctc2(rd, b); break;
            default: WARN("cop2 op %08x at %08x\n", op, cur_pc); break;
            }
            break;
        }
        case 0x20: *cycles += 4; WATCH(a + simm); LOAD(rt, op_sext8(rd8(a + simm))); break;
        case 0x21: *cycles += 4; WATCH(a + simm); LOAD(rt, op_sext16(rd16(a + simm))); break;
        case 0x22: *cycles += 4; LOAD(rt, op_lwl(a + simm, b_fwd)); break;
        case 0x23: *cycles += 4; WATCH(a + simm); LOAD(rt, rd32(a + simm)); break;
        case 0x24: *cycles += 4; WATCH(a + simm); LOAD(rt, rd8(a + simm)); break;
        case 0x25: *cycles += 4; WATCH(a + simm); LOAD(rt, rd16(a + simm)); break;
        case 0x26: *cycles += 4; LOAD(rt, op_lwr(a + simm, b_fwd)); break;
        case 0x28: *cycles += 1; WWATCH(a + simm); wr8(a + simm, b); break;
        case 0x29: *cycles += 1; WWATCH(a + simm); wr16(a + simm, b); break;
        case 0x2a: *cycles += 1; op_swl(a + simm, b); break;
        case 0x2b: *cycles += 1; WWATCH(a + simm); wr32(a + simm, b); break;
        case 0x2e: *cycles += 1; op_swr(a + simm, b); break;
        case 0x32: *cycles += 4; gte_lwc2(rt, rd32(a + simm)); break;
        case 0x3a: *cycles += 1; wr32(a + simm, gte_swc2(rt)); break;
        default: WARN("illegal opcode %08x at %08x\n", op, cur_pc); break;
        }
        { u32 o6 = op >> 26; dslot = (o6 >= 1 && o6 <= 7) || (o6 == 0 && ((op & 63) == 8 || (op & 63) == 9)); }
    }
}
