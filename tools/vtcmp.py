import sys, struct
a = open(sys.argv[1],'rb').read(); b = open(sys.argv[2],'rb').read()
n = min(len(a), len(b)) // 12
print('records', len(a)//12, len(b)//12)
for i in range(n):
    ra = struct.unpack_from('<III', a, i*12); rb = struct.unpack_from('<III', b, i*12)
    if ra != rb:
        print('first diff at record', i)
        for k in range(max(0,i-3), min(n, i+3)):
            x = struct.unpack_from('<III', a, k*12); y = struct.unpack_from('<III', b, k*12)
            print(k, 'interp pc=%08x cyc=%d h=%08x' % x, '| recomp pc=%08x cyc=%d h=%08x' % y)
        break
else:
    print('identical over', n, 'records')
