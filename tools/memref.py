"""只读：在游戏内存里找等于某些 8 字节值的位置（8 字节对齐），并打印每处前后的 qword（指针指向可读名字 / 招式时标出来）。
用法: memref.py <值hex>... [--lo 地址] [--hi 地址] [--ctx 前后各几个qword=6] [--max 每个值最多打印几处=12]"""
import struct, sys, re, ctypes, ctypes.wintypes as W
from golist import g

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', W.DWORD),
                ('PartitionId', W.WORD), ('RegionSize', ctypes.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
args = sys.argv[1:]
opt = {'--lo': 0x7ff000000000, '--hi': g.base, '--ctx': 6, '--max': 12}
for k in list(opt):
    if k in args:
        i = args.index(k); opt[k] = int(args[i + 1], 16 if k in ('--lo', '--hi') else 10); del args[i:i + 2]
vals = [int(a, 16) for a in args]
rx = re.compile(b'|'.join(re.escape(struct.pack('<Q', v)) for v in vals))
hits = {v: [] for v in vals}
a, m = opt['--lo'], MBI()
while a < opt['--hi'] and k32.VirtualQueryEx(g.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
    base, size = m.BaseAddress or 0, m.RegionSize
    if m.State == 0x1000 and (m.Protect & 0xff) in (0x02, 0x04, 0x20, 0x40) and not (m.Protect & 0x100):
        for off in range(0, size, 1 << 24):
            b = g.read(base + off, min(1 << 24, size - off))
            if not b: continue
            for h in rx.finditer(b):
                if (h.start() & 7) == 0: hits[struct.unpack('<Q', h.group())[0]].append(base + off + h.start())
    a = base + size


def note(v):
    if not (0x10000 < v < 0x7fffffffffff): return ''
    b = g.read(v, 0x48)
    if not b: return ''
    s = b.split(b'\0')[0]
    if len(s) >= 4 and all(32 <= c < 127 for c in s): return f'  "{s.decode()}"'
    return f'  → {b[:16].hex(" ")}'


for v, hs in hits.items():
    print(f'=== {v:#x}：{len(hs)} 处')
    for h in hs[:opt['--max']]:
        print(f'--- @ {h:#x}')
        n = opt['--ctx']; b = g.read(h - 8 * n, 8 * (2 * n + 1)) or b''
        for i in range(len(b) // 8):
            x = struct.unpack_from('<Q', b, i * 8)[0]
            print(f'   {h - 8 * n + i * 8:#x} {"+" if i >= n else "-"}{abs(i - n) * 8:#04x}  {x:016x}{note(x)}{"   <==" if i == n else ""}')
