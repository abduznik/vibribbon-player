#!/usr/bin/env python3
"""Find code referencing a given absolute data address (lui+offset pairs, gp-relative, jal targets)."""
import sys, struct
sys.path.insert(0,'tools')
from mips import *
e = load_exe(sys.argv[1])
targets = [int(x,16) for x in sys.argv[2:]]
n = e.size//4
ins=[Insn(e.addr+4*i, struct.unpack_from('<I', e.text, 4*i)[0]) for i in range(n)]
for i,x in enumerate(ins):
    if x.name == 'lui':
        base = x.imm << 16
        for j in range(i+1, min(n, i+8)):
            y = ins[j]
            if y.name in ('lw','lh','lhu','lb','lbu','sw','sh','sb','addiu','ori','addi') and y.rs == x.rt:
                if y.name == 'ori': val = base | y.imm
                else: val = (base + y.simm()) & 0xffffffff
                if val in targets:
                    print(f'{y.addr:08x}: {y.text()}   (target {val:08x}, lui at {x.addr:08x})')
    if x.name == 'jal' and x.jump_target() in targets:
        print(f'{x.addr:08x}: {x.text()}')
