#!/usr/bin/env python3
"""Extract files from a MODE2/2352 PS1 data track (ISO9660, form1 user data at +24)."""
import sys, os, struct

def sector_user(f, lba):
    f.seek(lba*2352)
    s = f.read(2352)
    mode = s[15]
    if mode == 2:
        sub = s[16:20]
        # form2 flag = submode bit 5
        if sub[2] & 0x20:
            return s[24:24+2324]
        return s[24:24+2048]
    return s[16:16+2048]

def read_file(f, lba, size):
    out = bytearray()
    n = (size+2047)//2048
    for i in range(n):
        out += sector_user(f, lba+i)
    return bytes(out[:size])

def parse_dir(f, lba, size, path, out):
    data = read_file(f, lba, size)
    i = 0
    while i < len(data):
        l = data[i]
        if l == 0:
            i = ((i//2048)+1)*2048
            continue
        rec = data[i:i+l]
        elba = struct.unpack_from('<I', rec, 2)[0]
        esize = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33+nlen].decode('ascii', 'replace')
        i += l
        if name in ('\x00', '\x01'):
            continue
        full = path + '/' + name
        if flags & 2:
            parse_dir(f, elba, esize, full, out)
        else:
            out.append((full, elba, esize))

def main():
    track, outdir = sys.argv[1], sys.argv[2]
    f = open(track, 'rb')
    pvd = sector_user(f, 16)
    assert pvd[1:6] == b'CD001'
    root = pvd[156:156+34]
    rlba = struct.unpack_from('<I', root, 2)[0]
    rsize = struct.unpack_from('<I', root, 10)[0]
    files = []
    parse_dir(f, rlba, rsize, '', files)
    for full, lba, size in files:
        print(f"{full:40s} lba={lba:6d} size={size}")
        name = full.split(';')[0]
        p = os.path.join(outdir, name.lstrip('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(read_file(f, lba, size))

main()
