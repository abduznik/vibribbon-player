#!/usr/bin/env python3
"""Static recompiler: PS-X EXE (MIPS R3000A) -> C.

Generated code works on the same machine state as the interpreter (cpu.r[], RAM, hardware), keeps
delay-slot / load-delay semantics, charges cycles with the interpreter's cost model and checks for
hardware events at function entries and taken backward branches (so interpreter and recompiled code
can be run in lock step for verification).
"""
import sys, os, re, struct, zlib, argparse, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Insn, Exe, load_exe, s16, REGS

LOAD_OPS = ('lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr')
STORE_OPS = ('sb', 'sh', 'swl', 'sw', 'swr')
DELAYED_WB = LOAD_OPS + ('mfc0', 'mfc2', 'cfc2')      # results land one instruction late
BIOS_VECS = (0xA0, 0xB0, 0xC0)


def cost(I):
    n = I.name
    c = 1
    if n in LOAD_OPS or n == 'lwc2':
        c += 4
    elif n in STORE_OPS or n == 'swc2':
        c += 1
    elif n in ('mult', 'multu'):
        c += 5
    elif n in ('div', 'divu'):
        c += 30
    return c


def reads(I):
    n = I.name
    rs, rt = I.rs, I.rt
    if n in ('sll', 'srl', 'sra'):
        return {rt}
    if n in ('sllv', 'srlv', 'srav', 'add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu',
             'mult', 'multu', 'div', 'divu', 'beq', 'bne'):
        return {rs, rt}
    if n in ('jr', 'jalr', 'mthi', 'mtlo', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal',
             'addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lwc2', 'swc2'):
        return {rs}
    if n in ('lb', 'lh', 'lw', 'lbu', 'lhu'):
        return {rs}
    if n in ('lwl', 'lwr'):
        return {rs}                 # rt is only merged with the in-flight value
    if n in STORE_OPS:
        return {rs, rt}
    if n in ('mtc0', 'mtc2', 'ctc2', 'ctc0'):
        return {rt}
    return set()


def writes(I):
    n = I.name
    if n in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor',
             'slt', 'sltu', 'mfhi', 'mflo', 'jalr'):
        return I.rd
    if n in ('addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lui') + LOAD_OPS + ('mfc0', 'mfc2', 'cfc2'):
        return I.rt
    if n in ('jal', 'bltzal', 'bgezal'):
        return 31
    return None


def is_control(I):
    return I.has_delay()


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = set()
        self.leaders = set()
        self.tables = {}        # jr address -> list of target addresses
        self.calls = set()
        self.bad = None


class Recomp:
    def __init__(self, exe, name, extra_entries=(), verbose=False, patched=()):
        self.patched = set(patched)
        self.exe = exe
        self.name = name
        self.verbose = verbose
        self.n = exe.size // 4
        self.words = struct.unpack_from('<%dI' % self.n, exe.text, 0)
        self.cache = {}
        self.entries = set(extra_entries)
        self.entries.add(exe.pc)
        self.funcs = {}
        self.bad_entries = {}
        self.base = re.sub(r'\W', '_', name)
        self.hazards = []

    # ---------------------------------------------------------------- decoding
    def in_text(self, a):
        return self.exe.addr <= a < self.exe.end and (a & 3) == 0

    def ins(self, a):
        I = self.cache.get(a)
        if I is None:
            if not self.in_text(a):
                return None
            I = Insn(a, self.words[(a - self.exe.addr) >> 2])
            self.cache[a] = I
        return I

    def word(self, a):
        if self.in_text(a):
            return self.words[(a - self.exe.addr) >> 2]
        return None

    # ---------------------------------------------------------------- seeds
    def looks_like_function_start(self, a):
        I = self.ins(a)
        if I is None or I.name == '.illegal':
            return False
        # typical prologue or a leaf starting with something sensible, preceded by a return/nop/padding
        if I.name == 'addiu' and I.rt == 29 and I.rs == 29 and I.simm() < 0:
            return True
        prev = self.ins(a - 4)
        prev2 = self.ins(a - 8)
        if prev is not None and prev2 is not None:
            if prev.raw == 0 and prev2.name == 'jr' and prev2.rs == 31:
                return True
            if prev2.name == 'jr' and prev2.rs == 31 and prev.name != '.illegal':
                return True
        return False

    def scan_constant_pointers(self):
        """lui+addiu/ori address constants and data words that point at plausible function starts."""
        found = set()
        n = self.n
        for i in range(n):
            I = self.ins(self.exe.addr + 4 * i)
            if I.name != 'lui':
                continue
            base = I.imm << 16
            for j in range(i + 1, min(n, i + 6)):
                J = self.ins(self.exe.addr + 4 * j)
                if J.name in ('addiu', 'ori') and J.rs == I.rt:
                    v = (base | J.imm) if J.name == 'ori' else (base + J.simm()) & 0xFFFFFFFF
                    if self.in_text(v) and self.looks_like_function_start(v):
                        found.add(v)
                    break
                if J.name == 'lui' and J.rt == I.rt:
                    break
        for i in range(n):
            w = self.words[i]
            if (w >> 28) == 8 and self.in_text(w) and self.looks_like_function_start(w):
                a = self.exe.addr + 4 * i
                # only words in data: skip if the word itself is a plausible instruction in code context
                found.add(w)
        return found

    # ---------------------------------------------------------------- jump tables
    def resolve_table(self, jr_addr, rs):
        """Find a switch table feeding `jr rs`. Returns list of targets or None."""
        start = max(self.exe.addr, jr_addr - 4 * 28)
        consts = {}        # reg -> int constant
        based = {}         # reg -> constant base (const + unknown index)
        table = None
        bound = None
        a = start
        while a < jr_addr:
            I = self.ins(a)
            n = I.name
            if n == 'lui':
                consts[I.rt] = I.imm << 16
                based.pop(I.rt, None)
            elif n in ('addiu', 'ori') and I.rs in consts:
                v = (consts[I.rs] | I.imm) if n == 'ori' else (consts[I.rs] + I.simm()) & 0xFFFFFFFF
                consts[I.rt] = v
                based.pop(I.rt, None)
            elif n == 'addu':
                if I.rs in consts and I.rt not in consts:
                    based[I.rd] = consts[I.rs]
                    consts.pop(I.rd, None)
                elif I.rt in consts and I.rs not in consts:
                    based[I.rd] = consts[I.rt]
                    consts.pop(I.rd, None)
                elif I.rs in consts and I.rt in consts:
                    consts[I.rd] = (consts[I.rs] + consts[I.rt]) & 0xFFFFFFFF
                else:
                    consts.pop(I.rd, None)
                    based.pop(I.rd, None)
            elif n == 'lw' and I.rs in based:
                table = (based[I.rs] + I.simm()) & 0xFFFFFFFF
                consts.pop(I.rt, None)
                based.pop(I.rt, None)
            elif n == 'sltiu':
                bound = I.imm
                w = writes(I)
                consts.pop(w, None)
                based.pop(w, None)
            else:
                w = writes(I)
                if w is not None:
                    consts.pop(w, None)
                    based.pop(w, None)
            a += 4
        if table is None:
            return None
        targets = []
        limit = bound if bound is not None and 0 < bound <= 1024 else 512
        for k in range(limit):
            t = self.word(table + 4 * k)
            if t is None or not self.in_text(t) or (t >> 28) != 8:
                if bound is None:
                    break
                return None
            targets.append(t)
        if not targets:
            return None
        return targets

    # ---------------------------------------------------------------- function discovery
    def explore(self, entry):
        f = Func(entry)
        work = [entry]
        f.leaders.add(entry)
        seen = f.insns
        while work:
            pc = work.pop()
            while True:
                if pc in seen:
                    break
                I = self.ins(pc)
                if I is None:
                    f.bad = 'left text at %08x' % pc
                    return f
                if I.name in ('.illegal', 'bc0', 'bc2', 'cfc0', 'ctc0'):
                    f.bad = 'unsupported opcode at %08x' % pc
                    return f
                seen.add(pc)
                n = I.name
                if I.has_delay():
                    D = self.ins(pc + 4)
                    if D is None or D.name in ('.illegal', 'bc0', 'bc2', 'cfc0', 'ctc0') or D.has_delay():
                        f.bad = 'bad delay slot at %08x' % pc
                        return f
                    seen.add(pc + 4)
                if n in ('bltzal', 'bgezal'):
                    f.bad = 'branch-and-link at %08x' % pc
                    return f
                if I.is_branch():
                    t = I.branch_target()
                    if t in self.entries and t != entry:
                        pass                    # conditional tail call
                    elif self.in_text(t):
                        f.leaders.add(t)
                        work.append(t)
                    f.leaders.add(pc + 8)
                    pc += 8
                    continue
                if n == 'j':
                    t = I.jump_target()
                    if t in self.entries and t != entry:
                        pass                    # tail call
                    elif self.in_text(t):
                        f.leaders.add(t)
                        work.append(t)
                    break
                if n == 'jal':
                    f.calls.add(I.jump_target())
                    f.leaders.add(pc + 8)
                    pc += 8
                    continue
                if n == 'jalr':
                    f.leaders.add(pc + 8)
                    pc += 8
                    continue
                if n == 'jr':
                    if I.rs != 31:
                        tg = self.resolve_table(pc, I.rs)
                        if tg:
                            f.tables[pc] = tg
                            for t in tg:
                                f.leaders.add(t)
                                work.append(t)
                    break
                pc += 4
        return f

    def discover(self):
        seeds = set(self.entries)
        seeds |= self.scan_constant_pointers()
        self.entries = set(s for s in seeds if self.in_text(s))
        while True:
            funcs, bad, new = {}, {}, set()
            for e in sorted(self.entries):
                f = self.explore(e)
                if f.bad:
                    bad[e] = f.bad
                else:
                    funcs[e] = f
                for c in f.calls:
                    if self.in_text(c) and c not in self.entries:
                        new.add(c)
            if not new:
                break
            self.entries |= new
        # functions whose code is rewritten at run time (self-modifying) stay with the interpreter
        for e in sorted(funcs):
            hit = funcs[e].insns & self.patched
            if hit:
                bad[e] = 'self-modified at %s' % ','.join('%08x' % a for a in sorted(hit))
                del funcs[e]
        self.funcs, self.bad_entries = funcs, bad

    # ---------------------------------------------------------------- code generation
    def label(self, a):
        return 'L_%08x' % a

    def fname(self, a):
        return '%s_f_%08x' % (self.base, a)

    def reg(self, n):
        return '0u' if n == 0 else 'R%d' % n

    def setr(self, n, expr):
        return '' if n == 0 else 'R%d = %s;' % (n, expr)

    def gen_alu(self, I):
        """C statement for a non-control, non-load instruction (reads the registers as they are now)."""
        n = I.name
        rs, rt = self.reg(I.rs), self.reg(I.rt)
        sh = I.sh
        imm = I.imm
        simm = I.simm() & 0xFFFFFFFF
        S = self.setr
        if n == 'sll':
            return '' if I.raw == 0 else S(I.rd, '%s << %d' % (rt, sh))
        if n == 'srl': return S(I.rd, '%s >> %d' % (rt, sh))
        if n == 'sra': return S(I.rd, '(u32)((s32)%s >> %d)' % (rt, sh))
        if n == 'sllv': return S(I.rd, '%s << (%s & 31)' % (rt, rs))
        if n == 'srlv': return S(I.rd, '%s >> (%s & 31)' % (rt, rs))
        if n == 'srav': return S(I.rd, '(u32)((s32)%s >> (%s & 31))' % (rt, rs))
        if n in ('add', 'addu'): return S(I.rd, '%s + %s' % (rs, rt))
        if n in ('sub', 'subu'): return S(I.rd, '%s - %s' % (rs, rt))
        if n == 'and': return S(I.rd, '%s & %s' % (rs, rt))
        if n == 'or': return S(I.rd, '%s | %s' % (rs, rt))
        if n == 'xor': return S(I.rd, '%s ^ %s' % (rs, rt))
        if n == 'nor': return S(I.rd, '~(%s | %s)' % (rs, rt))
        if n == 'slt': return S(I.rd, '(u32)((s32)%s < (s32)%s)' % (rs, rt))
        if n == 'sltu': return S(I.rd, '(u32)(%s < %s)' % (rs, rt))
        if n == 'mfhi': return S(I.rd, 'cpu.hi')
        if n == 'mflo': return S(I.rd, 'cpu.lo')
        if n == 'mthi': return 'cpu.hi = %s;' % rs
        if n == 'mtlo': return 'cpu.lo = %s;' % rs
        if n == 'mult': return 'op_mult(%s, %s);' % (rs, rt)
        if n == 'multu': return 'op_multu(%s, %s);' % (rs, rt)
        if n == 'div': return 'op_div(%s, %s);' % (rs, rt)
        if n == 'divu': return 'op_divu(%s, %s);' % (rs, rt)
        if n in ('addi', 'addiu'): return S(I.rt, '%s + 0x%xu' % (rs, simm))
        if n == 'slti': return S(I.rt, '(u32)((s32)%s < (s32)0x%xu)' % (rs, simm))
        if n == 'sltiu': return S(I.rt, '(u32)(%s < 0x%xu)' % (rs, simm))
        if n == 'andi': return S(I.rt, '%s & 0x%xu' % (rs, imm))
        if n == 'ori': return S(I.rt, '%s | 0x%xu' % (rs, imm))
        if n == 'xori': return S(I.rt, '%s ^ 0x%xu' % (rs, imm))
        if n == 'lui': return S(I.rt, '0x%xu' % (imm << 16))
        if n in STORE_OPS:
            addr = '%s + 0x%xu' % (rs, simm)
            if n == 'sb': return 'wr8(%s, %s);' % (addr, rt)
            if n == 'sh': return 'wr16(%s, %s);' % (addr, rt)
            if n == 'sw': return 'wr32(%s, %s);' % (addr, rt)
            if n == 'swl': return 'op_swl(%s, %s);' % (addr, rt)
            if n == 'swr': return 'op_swr(%s, %s);' % (addr, rt)
        if n == 'swc2': return 'wr32(%s + 0x%xu, gte_swc2(%d));' % (rs, simm, I.rt)
        if n == 'lwc2': return 'gte_lwc2(%d, rd32(%s + 0x%xu));' % (I.rt, rs, simm)
        if n == 'mtc0': return 'rt_cop0_write(%d, %s);' % (I.rd, rt)
        if n == 'mtc2': return 'gte_mtc2(%d, %s);' % (I.rd, rt)
        if n == 'ctc2': return 'gte_ctc2(%d, %s);' % (I.rd, rt)
        if n == 'cop2cmd': return 'gte_cmd(0x%x);' % (I.raw & 0x1FFFFFF)
        if n == 'rfe': return 'rt_rfe();'
        if n == 'syscall': return 'rt_syscall(0x%08xu);' % I.addr
        if n == 'break': return 'rt_break(0x%08xu);' % I.addr
        raise ValueError('cannot generate ' + I.text())

    def load_expr(self, I, merge=None):
        """Expression producing the value a delayed-writeback instruction delivers."""
        n = I.name
        rs = self.reg(I.rs)
        simm = I.simm() & 0xFFFFFFFF
        addr = '%s + 0x%xu' % (rs, simm)
        rtv = merge if merge is not None else self.reg(I.rt)
        if n == 'lb': return 'op_sext8(rd8(%s))' % addr
        if n == 'lh': return 'op_sext16(rd16(%s))' % addr
        if n == 'lw': return 'rd32(%s)' % addr
        if n == 'lbu': return 'rd8(%s)' % addr
        if n == 'lhu': return 'rd16(%s)' % addr
        if n == 'lwl': return 'op_lwl(%s, %s)' % (addr, rtv)
        if n == 'lwr': return 'op_lwr(%s, %s)' % (addr, rtv)
        if n == 'mfc0': return 'rt_cop0_read(%d)' % I.rd
        if n == 'mfc2': return 'gte_mfc2(%d)' % I.rd
        if n == 'cfc2': return 'gte_cfc2(%d)' % I.rd
        raise ValueError(n)

    # ---- function / block generation
    def gen_function(self, f):
        self.cur = f
        self.tmpn = 0
        leaders = sorted(l for l in f.leaders if l in f.insns)
        lead_set = set(leaders)
        out = ['void %s(void) {' % self.fname(f.entry),
               '    u32 ra0 = R31;',
               '    FUNC_ENTRY(0x%08xu);' % f.entry]
        for idx, L in enumerate(leaders):
            nxt = leaders[idx + 1] if idx + 1 < len(leaders) else None
            body = self.gen_block(f, L, lead_set, nxt)
            out.append('%s:;' % self.label(L))
            out.extend('    ' + s for s in body)
        out.append('}')
        return out

    def newtmp(self):
        self.tmpn += 1
        return 'ld%d' % self.tmpn

    def goto_label(self, f, t, lead_set, backward):
        """Statement(s) continuing at t inside function f (t is known to be a leader of f)."""
        chk = backward or (t in self.funcs and t != f.entry)
        s = 'goto %s;' % self.label(t)
        return '{ CHECK_EVENT(0x%08xu); %s }' % (t, s) if chk else s

    def transfer(self, f, t, lead_set, backward, tail=True):
        """Control transfers to static address t (no return to this point). Returns a statement."""
        if t in self.funcs and t != f.entry:
            return '{ %s(); return; }' % self.fname(t)
        if t in lead_set:
            return self.goto_label(f, t, lead_set, backward)
        return '{ rt_dyn_jump(0x%08xu); return; }' % t

    def hazard_check(self, f, I, D, succs):
        if D.name in DELAYED_WB and D.rt != 0:
            for s in succs:
                J = self.ins(s)
                if J is not None and D.rt in reads(J):
                    self.hazards.append('%08x: load in delay slot of "%s" feeds %08x "%s"' % (D.addr, I.text(), s, J.text()))

    def gen_block(self, f, L, lead_set, nxt):
        body = []
        pc = L
        pending = None            # (reg, tmp) of an unresolved delayed write-back
        consts = {}
        while True:
            I = self.ins(pc)
            if I.has_delay():
                D = self.ins(pc + 4)
                body.extend(self.gen_control(f, I, D, lead_set, pending, consts))
                return body
            body.append('cpu.cycles += %d;' % cost(I))
            n = I.name
            nxt_pc = pc + 4
            nxt_i = self.ins(nxt_pc) if (nxt_pc in f.insns and nxt_pc not in lead_set) else None
            if n in DELAYED_WB:
                t = self.newtmp()
                merge = pending[1] if (pending and n in ('lwl', 'lwr') and I.rt == pending[0]) else None
                body.append('u32 %s = %s;' % (t, self.load_expr(I, merge)))
                if pending:
                    body.append('R%d = %s;' % pending)
                pending = None
                if I.rt != 0:
                    if nxt_i is not None and I.rt in reads(nxt_i):
                        pending = (I.rt, t)
                    else:
                        body.append('R%d = %s;' % (I.rt, t))
                        if nxt_i is None and nxt_pc in lead_set:
                            self.note_fallthrough_hazard(I, nxt_pc)
                elif True:
                    body.append('(void)%s;' % t)
            else:
                s = self.gen_alu(I)
                if s:
                    body.append(s)
                if pending:
                    if writes(I) != pending[0]:
                        body.append('R%d = %s;' % pending)
                    pending = None
            self.track_const(I, consts)
            pc = nxt_pc
            if pc in lead_set or pc not in f.insns:
                break
        # fell out of the block: continue at pc
        if pc in lead_set:
            body.append(self.goto_label(f, pc, lead_set, False) if (pc in self.funcs and pc != f.entry) else
                        ('goto %s;' % self.label(pc) if pc != nxt else ''))
        else:
            body.append('{ rt_dyn_jump(0x%08xu); return; }' % pc)
        return [b for b in body if b]

    def note_fallthrough_hazard(self, I, nxt_pc):
        J = self.ins(nxt_pc)
        if J is not None and I.rt in reads(J):
            self.hazards.append('%08x: load "%s" feeds block leader %08x "%s"' % (I.addr, I.text(), nxt_pc, J.text()))

    def track_const(self, I, consts):
        n = I.name
        if n == 'lui':
            consts[I.rt] = I.imm << 16
            return
        if n in ('addiu', 'ori') and (I.rs == 0 or I.rs in consts):
            base = 0 if I.rs == 0 else consts[I.rs]
            consts[I.rt] = ((base | I.imm) if n == 'ori' else (base + I.simm())) & 0xFFFFFFFF
            return
        w = writes(I)
        if w is not None:
            consts.pop(w, None)

    def gen_delay(self, D, pending):
        """Delay-slot statements. A pending load from before the branch lands first; a load in the slot itself
        is applied immediately (a possible hazard is reported by the caller)."""
        out = []
        if pending:
            out.append('R%d = %s;' % pending)
        out.append('cpu.cycles += %d;' % cost(D))
        if D.name in DELAYED_WB:
            e = self.load_expr(D)
            out.append('(void)(%s);' % e if D.rt == 0 else 'R%d = %s;' % (D.rt, e))
        else:
            s = self.gen_alu(D)
            if s:
                out.append(s)
        return out

    def gen_control(self, f, I, D, lead_set, pending, consts):
        out = ['cpu.cycles += %d;' % cost(I)]
        n = I.name
        after = I.addr + 8

        def fall():
            if after in lead_set:
                return [self.goto_label(f, after, lead_set, False)] if (after in self.funcs and after != f.entry) else ['goto %s;' % self.label(after)]
            return ['{ rt_dyn_jump(0x%08xu); return; }' % after]

        if I.is_branch():
            t = I.branch_target()
            rs, rt = self.reg(I.rs), self.reg(I.rt)
            cond = {'beq': '%s == %s' % (rs, rt), 'bne': '%s != %s' % (rs, rt), 'blez': '(s32)%s <= 0' % rs,
                    'bgtz': '(s32)%s > 0' % rs, 'bltz': '(s32)%s < 0' % rs, 'bgez': '(s32)%s >= 0' % rs}[n]
            self.hazard_check(f, I, D, [t, after])
            out.append('{')
            out.append('    u32 c = %s;' % cond)
            for s in self.gen_delay(D, pending):
                out.append('    ' + s)
            out.append('    if (c) %s' % self.transfer(f, t, lead_set, t <= I.addr))
            out.append('}')
            out.extend(fall())
            return out

        if n == 'j':
            t = I.jump_target()
            self.hazard_check(f, I, D, [t])
            out.append('{')
            for s in self.gen_delay(D, pending):
                out.append('    ' + s)
            out.append('    ' + self.transfer(f, t, lead_set, t <= I.addr))
            out.append('}')
            return out

        if n == 'jal':
            t = I.jump_target()
            out.append('{')
            if pending:
                out.append('    R%d = %s;' % pending)
                pending = None
            out.append('    R31 = 0x%08xu;' % after)
            for s in self.gen_delay(D, pending):
                out.append('    ' + s)
            out.append('    %s();' % self.fname(t) if t in self.funcs else '    rt_call(0x%08xu);' % t)
            out.append('}')
            out.extend(fall())
            return out

        if n == 'jalr':
            out.append('{')
            out.append('    u32 t = %s;' % self.reg(I.rs))
            if pending:
                out.append('    R%d = %s;' % pending)
                pending = None
            if I.rd:
                out.append('    R%d = 0x%08xu;' % (I.rd, after))
            for s in self.gen_delay(D, pending):
                out.append('    ' + s)
            out.append('    rt_dyn_call(t);')
            out.append('}')
            out.extend(fall())
            return out

        if n == 'jr':
            out.append('{')
            out.append('    u32 t = %s;' % self.reg(I.rs))
            for s in self.gen_delay(D, pending):
                out.append('    ' + s)
            cv = consts.get(I.rs)
            tbl = f.tables.get(I.addr)
            if I.rs == 31:
                out.append('    RET_TO(t);')
            elif cv is not None and (cv & 0x1FFFFFFF) in BIOS_VECS:
                out.append('    bios_call(0x%x, R9);' % (cv & 0x1FFFFFFF))
                out.append('    RET_TO(R31);')
            elif tbl:
                out.append('    switch (t) {')
                for tg in sorted(set(tbl)):
                    if tg in lead_set:
                        out.append('    case 0x%08xu: %s' % (tg, self.goto_label(f, tg, lead_set, tg <= I.addr)))
                out.append('    default: rt_dyn_jump(t); return;')
                out.append('    }')
            else:
                out.append('    rt_dyn_jump(t); return;')
            out.append('}')
            return out
        raise ValueError('control ' + n)

    # ---------------------------------------------------------------- emit translation units
    def emit(self, outdir, nchunks=8):
        os.makedirs(outdir, exist_ok=True)
        self.hazards = []
        while True:
            bodies, failed = [], False
            for e in sorted(self.funcs):
                try:
                    bodies.append((e, self.gen_function(self.funcs[e])))
                except Exception as ex:      # noqa
                    self.bad_entries[e] = 'codegen: %s' % ex
                    del self.funcs[e]
                    failed = True
                    break
            if not failed:
                break
            self.hazards = []                # a function dropped out: callers must be regenerated
        base = self.base
        header = ['/* generated by tools/recomp.py from %s - do not edit */' % self.name,
                  '#include "recomp_rt.h"', '']
        protos = ['void %s(void);' % self.fname(e) for e in sorted(self.funcs)]
        per = max(1, (len(bodies) + nchunks - 1) // nchunks)
        files = []
        for ci in range(0, len(bodies), per):
            chunk = bodies[ci:ci + per]
            fn = os.path.join(outdir, '%s_%02d.c' % (base, ci // per))
            lines = header + protos + ['']
            for e, body in chunk:
                lines.extend(body)
                lines.append('')
            open(fn, 'w').write('\n'.join(lines))
            files.append(fn)
        crc = zlib.crc32(self.exe.text) & 0xFFFFFFFF
        tfn = os.path.join(outdir, '%s_table.c' % base)
        t = header + protos
        t.append('static const RecompEntry tbl[] = {')
        for e in sorted(self.funcs):
            t.append('    {0x%08xu, %s},' % (e, self.fname(e)))
        t.append('};')
        covered = set()
        for f in self.funcs.values():
            covered |= f.insns
        lo = min(covered) & 0x1FFFFFFF
        hi = (max(covered) + 4) & 0x1FFFFFFF
        nwords = (hi - lo) // 4
        bits = [0] * ((nwords + 31) // 32)
        for a in covered:
            w = ((a & 0x1FFFFFFF) - lo) // 4
            bits[w >> 5] |= 1 << (w & 31)
        t.append('static const u32 cover[] = {%s};' % ','.join('0x%x' % b for b in bits))
        t.append('const RecompOverlay ov_%s = {"%s", 0x%08xu, 0x%xu, 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, tbl, %d, 0x%xu, 0x%xu, cover};' % (
            base, self.name, self.exe.addr, self.exe.size, crc, self.exe.pc, self.exe.gp, self.exe.sp_base, len(self.funcs), lo, hi - lo))
        open(tfn, 'w').write('\n'.join(t) + '\n')
        files.append(tfn)
        return files, crc


def write_overlays(outdir):
    import re
    names = []
    for fn in sorted(os.listdir(outdir)):
        if fn.endswith('_table.c'):
            m = re.search(r'const RecompOverlay (ov_\w+) =', open(os.path.join(outdir, fn)).read())
            if m:
                names.append(m.group(1))
    lines = ['/* generated by tools/recomp.py - do not edit */', '#include "psx.h"']
    lines += ['extern const RecompOverlay %s;' % n for n in names]
    lines.append('const RecompOverlay *const g_overlays[] = {%s};' % ', '.join(['&%s' % n for n in names] + ['0']))
    lines.append('const int g_overlay_count = %d;' % len(names))
    open(os.path.join(outdir, 'overlays.c'), 'w').write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--name', required=True)
    ap.add_argument('--out', default='gen')
    ap.add_argument('--entries', action='append', default=[], help='file with hex entry addresses')
    ap.add_argument('--chunks', type=int, default=8)
    ap.add_argument('--patched', action='append', default=[], help='file with hex addresses of words written at run time')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    exe = load_exe(a.exe)
    extra = set()
    for fn in a.entries:
        for line in open(fn):
            line = line.strip()
            if line:
                extra.add(int(line, 16))
    patched = set()
    for fn in a.patched:
        for line in open(fn):
            line = line.strip()
            if line:
                patched.add(int(line, 16))
    r = Recomp(exe, a.name, extra, a.v, patched)
    r.discover()
    nd = len(r.funcs)
    files, crc = r.emit(a.out, a.chunks)
    write_overlays(a.out)
    print('%s: %d functions found, %d recompiled, %d rejected (crc %08x)' % (a.name, nd, len(r.funcs), len(r.bad_entries), crc))
    if a.v:
        for e, why in sorted(r.bad_entries.items()):
            print('  rejected %08x: %s' % (e, why))
    for h in r.hazards:
        print('  hazard:', h)


if __name__ == '__main__':
    main()
