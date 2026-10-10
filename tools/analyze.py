#!/usr/bin/env python3
import sys, collections
sys.path.insert(0, 'tools')
from mips import *
from biosnames import TABLES

def main(path):
    e = load_exe(path)
    n = e.size // 4
    ins = [Insn(e.addr + 4*i, struct.unpack_from('<I', e.text, 4*i)[0]) for i in range(n)]
    # BIOS stubs: addiu/ori t2,zero,0xA0 ; jr t2 ; addiu t1,zero,N   (any order of the li's around jr)
    stubs = {}
    for i in range(n-2):
        a, b, c = ins[i], ins[i+1], ins[i+2]
        if a.name in ('addiu','ori') and a.rs == 0 and a.rt == 10 and a.imm in (0xA0,0xB0,0xC0) \
           and b.name == 'jr' and b.rs == 10 and c.name in ('addiu','ori') and c.rs == 0 and c.rt == 9:
            stubs[a.addr] = (a.imm, c.imm)
    print(f'{path}: {n} words, {len(stubs)} BIOS stubs')
    for addr, (t, num) in sorted(stubs.items()):
        print(f'  {addr:08x}: {t:02X}h/{num:02X}h {TABLES[t].get(num,"?")}')
    # hw refs
    hw = collections.Counter()
    for i in range(n):
        x = ins[i]
        if x.name == 'lui' and x.imm == 0x1f80:
            hw[x.addr] += 1
    print('lui 0x1f80 sites:', len(hw))
    cop2 = collections.Counter()
    for x in ins:
        if x.name == 'cop2cmd':
            cop2[x.raw & 0x3f] += 1
    print('cop2 cmds (funct -> count):', {hex(k): v for k, v in sorted(cop2.items())})
    nm = collections.Counter(x.name for x in ins if x.name in ('mfc2','mtc2','cfc2','ctc2','lwc2','swc2','mfc0','mtc0','rfe','syscall','break','lwl','lwr','swl','swr','div','divu','mult','multu','.illegal'))
    print(dict(nm))

main(sys.argv[1])
