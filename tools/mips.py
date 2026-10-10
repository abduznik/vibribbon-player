#!/usr/bin/env python3
"""MIPS R3000A (PS1) instruction decoder / disassembler and PS-X EXE helpers."""
import struct, sys

REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
        't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
        't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Exe:
    def __init__(self, data):
        assert data[:8] == b'PS-X EXE', 'not a PS-X EXE'
        (self.pc, self.gp, self.addr, self.size) = struct.unpack_from('<IIII', data, 0x10)
        (self.d_addr, self.d_size, self.b_addr, self.b_size) = struct.unpack_from('<IIII', data, 0x20)
        (self.sp_base, self.sp_off) = struct.unpack_from('<II', data, 0x30)
        self.text = data[0x800:0x800 + self.size]
        self.data = data

    @property
    def end(self):
        return self.addr + self.size

    def word(self, a):
        o = (a & 0x1FFFFFFF) - (self.addr & 0x1FFFFFFF)
        if 0 <= o <= len(self.text) - 4:
            return struct.unpack_from('<I', self.text, o)[0]
        return None

    def contains(self, a):
        return self.addr <= (a | 0) < self.end


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ('addr', 'raw', 'op', 'rs', 'rt', 'rd', 'sh', 'fn', 'imm', 'tgt', 'name')

    def __init__(self, addr, raw):
        self.addr = addr
        self.raw = raw
        self.op = raw >> 26
        self.rs = (raw >> 21) & 31
        self.rt = (raw >> 16) & 31
        self.rd = (raw >> 11) & 31
        self.sh = (raw >> 6) & 31
        self.fn = raw & 63
        self.imm = raw & 0xFFFF
        self.tgt = raw & 0x3FFFFFF
        self.name = self._name()

    def simm(self):
        return s16(self.imm)

    def _name(self):
        op, fn = self.op, self.fn
        if op == 0:
            return {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr',
                    12: 'syscall', 13: 'break', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo',
                    24: 'mult', 25: 'multu', 26: 'div', 27: 'divu', 32: 'add', 33: 'addu', 34: 'sub',
                    35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}.get(fn, '.illegal')
        if op == 1:
            r = self.rt
            if r == 0: return 'bltz'
            if r == 1: return 'bgez'
            if r == 16: return 'bltzal'
            if r == 17: return 'bgezal'
            # PS1: other rt values behave as bltz/bgez depending on bit 0
            return 'bgez' if r & 1 else 'bltz'
        if op in (16, 18):
            c = 'cop0' if op == 16 else 'cop2'
            if self.rs == 0: return 'mfc' + c[3]
            if self.rs == 2: return 'cfc' + c[3]
            if self.rs == 4: return 'mtc' + c[3]
            if self.rs == 6: return 'ctc' + c[3]
            if op == 16 and self.rs == 16 and fn == 16: return 'rfe'
            if op == 18 and self.rs >= 16: return 'cop2cmd'
            if self.rs == 8: return 'bc' + c[3]
            return '.illegal'
        names = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu',
                 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui',
                 32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr',
                 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr', 50: 'lwc2', 58: 'swc2'}
        return names.get(op, '.illegal')

    # classification --------------------------------------------------------
    def is_branch(self):
        return self.name in ('beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal')

    def is_jump(self):
        return self.name in ('j', 'jal', 'jr', 'jalr')

    def has_delay(self):
        return self.is_branch() or self.is_jump()

    def branch_target(self):
        return (self.addr + 4 + (self.simm() << 2)) & 0xFFFFFFFF

    def jump_target(self):
        return ((self.addr + 4) & 0xF0000000) | (self.tgt << 2)

    def is_link(self):
        return self.name in ('jal', 'jalr', 'bltzal', 'bgezal')

    def text(self):
        n = self.name
        R = lambda i: '$' + REGS[i]
        if n == 'sll' and self.raw == 0:
            return 'nop'
        if n in ('sll', 'srl', 'sra'):
            return f'{n} {R(self.rd)}, {R(self.rt)}, {self.sh}'
        if n in ('sllv', 'srlv', 'srav'):
            return f'{n} {R(self.rd)}, {R(self.rt)}, {R(self.rs)}'
        if n == 'jr': return f'jr {R(self.rs)}'
        if n == 'jalr': return f'jalr {R(self.rd)}, {R(self.rs)}'
        if n in ('syscall', 'break'): return f'{n} 0x{(self.raw >> 6) & 0xFFFFF:x}'
        if n in ('mfhi', 'mflo'): return f'{n} {R(self.rd)}'
        if n in ('mthi', 'mtlo'): return f'{n} {R(self.rs)}'
        if n in ('mult', 'multu', 'div', 'divu'): return f'{n} {R(self.rs)}, {R(self.rt)}'
        if n in ('add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu'):
            return f'{n} {R(self.rd)}, {R(self.rs)}, {R(self.rt)}'
        if n in ('j', 'jal'): return f'{n} 0x{self.jump_target():08x}'
        if n in ('beq', 'bne'): return f'{n} {R(self.rs)}, {R(self.rt)}, 0x{self.branch_target():08x}'
        if n in ('blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'):
            return f'{n} {R(self.rs)}, 0x{self.branch_target():08x}'
        if n in ('addi', 'addiu', 'slti', 'sltiu'):
            return f'{n} {R(self.rt)}, {R(self.rs)}, {self.simm()}'
        if n in ('andi', 'ori', 'xori'):
            return f'{n} {R(self.rt)}, {R(self.rs)}, 0x{self.imm:x}'
        if n == 'lui': return f'lui {R(self.rt)}, 0x{self.imm:x}'
        if n in ('lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr', 'sb', 'sh', 'swl', 'sw', 'swr'):
            return f'{n} {R(self.rt)}, {self.simm()}({R(self.rs)})'
        if n in ('lwc2', 'swc2'): return f'{n} $c2_{self.rt}, {self.simm()}({R(self.rs)})'
        if n in ('mfc0', 'cfc0', 'mtc0', 'ctc0'): return f'{n} {R(self.rt)}, $cop0_{self.rd}'
        if n in ('mfc2', 'cfc2', 'mtc2', 'ctc2'): return f'{n} {R(self.rt)}, $c2_{self.rd}'
        if n == 'cop2cmd': return f'cop2 0x{self.raw & 0x1FFFFFF:07x}'
        if n == 'rfe': return 'rfe'
        return f'{n} 0x{self.raw:08x}'


def load_exe(path):
    return Exe(open(path, 'rb').read())


def disasm(exe, start, count):
    for i in range(count):
        a = start + 4 * i
        w = exe.word(a)
        if w is None:
            break
        print(f'{a:08x}:  {w:08x}  {Insn(a, w).text()}')


if __name__ == '__main__':
    e = load_exe(sys.argv[1])
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else e.pc
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
    print(f'pc={e.pc:08x} gp={e.gp:08x} addr={e.addr:08x} size={e.size:x} bss={e.b_addr:08x}+{e.b_size:x} sp={e.sp_base:08x}')
    disasm(e, start, count)
