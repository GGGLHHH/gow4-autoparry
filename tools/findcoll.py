"""只读：按命中标志在游戏内存里反查攻击段（Collision）→ 谁指着它 → 属于哪个招式。
三遍扫描（只读 VirtualQueryEx + ReadProcessMemory）：
  1. 找 8 字节值 == 命中标志、且 (地址-0x48) 像个 Collision（On/Off 是 0..1.5 的 f16、On<=Off）的位置；
  2. 找指向这些 Collision 的指针（招式的 CollisionList 数组项，或别的结构里的 Collision 字段）；
  3. 找指向「数组项地址 - 8*i（i<24）」的指针，且它所在位置 -0x10 处像个招式（+0x18 个数 > i、+0x28 动作数 < 400）。
用法: findcoll.py <命中标志hex>... [--lo 起始地址] [--hi 结束地址]"""
import ctypes, ctypes.wintypes as W, re, struct, sys, time
from golist import g

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', W.DWORD),
                ('PartitionId', W.WORD), ('RegionSize', ctypes.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
args = sys.argv[1:]
lo, hi = 0x7ff000000000, g.base
for opt in ('--lo', '--hi'):
    if opt in args:
        i = args.index(opt); v = int(args[i + 1], 16); del args[i:i + 2]
        lo, hi = (v, hi) if opt == '--lo' else (lo, v)
flags = [int(a, 16) for a in args]


def regions():
    a, m = lo, MBI()
    while a < hi and k32.VirtualQueryEx(g.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        base, size = m.BaseAddress or 0, m.RegionSize
        if m.State == 0x1000 and (m.Protect & 0xff) in (0x02, 0x04, 0x20, 0x40) and not (m.Protect & 0x100):
            yield base, size
        a = base + size


def scan(pats):
    """一遍扫完，返回 [(地址, 命中的 8 字节值)]，只要 8 字节对齐的"""
    rx = re.compile(b'|'.join(re.escape(struct.pack('<Q', p)) for p in pats)); out = []; total = 0
    for base, size in regions():
        for off in range(0, size, 1 << 24):
            n = min(1 << 24, size - off); b = g.read(base + off, n)
            if not b: continue
            total += n
            for m in rx.finditer(b):
                if (m.start() & 7) == 0: out.append((base + off + m.start(), struct.unpack('<Q', m.group())[0]))
    return out, total


def f16(b, o):
    return struct.unpack_from('<e', b, o)[0]


def q(a):
    b = g.read(a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


t0 = time.time()
hits, total = scan(flags)
colls = {}
for a, v in hits:
    c = a - 0x48; b = g.read(c, 0x90)
    if not b: continue
    on, off = f16(b, 0), f16(b, 2)
    if not (0 <= on <= off <= 1.5): continue
    colls[c] = (v, on, off, f16(b, 8), struct.unpack_from('<I', b, 0x44)[0])
print(f'第 1 遍：读了 {total >> 20} MB，{time.time() - t0:.1f} s；值命中 {len(hits)}，像攻击段的 {len(colls)}')
for c, (v, on, off, dmg, fl) in colls.items():
    print(f'  攻击段 {c:#x} HitFlags {v:016X} On {on:.3f} Off {off:.3f} 伤害 {dmg:g} Flags {fl:#x}')
if not colls: sys.exit()
refs, _ = scan(list(colls))
print(f'第 2 遍：指向这些攻击段的指针 {len(refs)} 处（{time.time() - t0:.1f} s）')
cand = {}
for r, c in refs:
    for i in range(24): cand.setdefault(r - 8 * i, []).append((r, c, i))
owners, _ = scan(list(cand))
print(f'第 3 遍：候选招式指针 {len(owners)} 处（{time.time() - t0:.1f} s）')
found = set()
for o, arr in owners:
    m = o - 0x10; h = g.read(m, 0x70)
    if not h: continue
    nb, nc, na = struct.unpack_from('<I', h, 8)[0], struct.unpack_from('<I', h, 0x18)[0], struct.unpack_from('<I', h, 0x28)[0]
    for r, c, i in cand[arr]:
        if i < nc <= 64 and na < 400 and nb < 400:
            found.add(r)
            print(f'  招式 {m:#x} 哈希 {struct.unpack_from("<Q", h, 0x40)[0]:016X} 分支 {nb} 攻击段 {nc} 动作 {na} → 第 {i} 段 = {c:#x}（HitFlags {colls[c][0]:016X}）')
for r, c in refs:
    if r not in found:
        ctx = g.read(r - 0x40, 0x48) or b''
        print(f'  不是招式攻击段列表的引用：{r:#x} → {c:#x}（HitFlags {colls[c][0]:016X}）；前 0x40 字节的 qword: ' +
              ' '.join(f'{x:x}' for x in struct.unpack_from('<8Q', ctx, 0)) if len(ctx) >= 0x40 else '')
