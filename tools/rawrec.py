"""只读：打印招式里指定类型记录的原始字节（每 8 字节一组），并标出哪些像指针。用法: rawrec.py <招式地址> <类型十六进制> [字节数=0x80]"""
import struct, sys
from golist import g
m = int(sys.argv[1], 16); want = int(sys.argv[2], 16); n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80
q = lambda a: struct.unpack('<Q', g.read(a, 8))[0]
arr, cnt = q(m + 0x20), q(m + 0x28) & 0xffffffff
for i in range(cnt):
    e = q(arr + i * 8); b = g.read(e, n)
    if (struct.unpack_from('<I', b, 0)[0] & 0xff) != want: continue
    print(f'记录 {i} @ {e:#x}')
    for o in range(0, n, 8):
        v = struct.unpack_from('<Q', b, o)[0]
        note = ''
        if 0x7ff000000000 <= v < 0x7fffffffffff:
            try:
                t = g.read(v, 16); note = '  → ' + t.hex(' ')
            except Exception:
                note = '  → (读不到)'
        print(f'  +{o:#04x} {v:016x}{note}')
