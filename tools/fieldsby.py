"""按字段名找字段描述项：exe 里每个可序列化结构都有一张表，每项 0x20 字节 = {名字指针, 名字指针, u64 A, u64 B}，
A = 偏移(16) | 大小(16) | 类型(16) | 结构号(16)。类型 0x1c = 指针（B 的第 16..31 位是指向的结构号），0x204 = 枚举位掩码。
用法: fieldsby.py <exe> <字段名>...      列出所有叫这个名字的字段在哪个结构、什么偏移
      fieldsby.py <exe> --struct <结构号hex>...   把这个结构的全部字段按偏移打出来
      fieldsby.py <exe> --ptrto <结构号hex>...    哪些结构里有指向这个结构的指针字段"""
import struct, sys, pefile
args = sys.argv[1:]
pe = pefile.PE(args[0], fast_load=True); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
data = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Name.rstrip(b'\0') in (b'.data', b'.rdata')]


def cstr(q):
    r = q - base
    if not (0x1000 <= r < len(img)): return None
    e = img.find(b'\0', r, r + 120); t = img[r:e] if e > r else b''
    return t.decode() if len(t) >= 2 and all(32 <= c < 127 for c in t) else None


def entries():
    """扫 .data/.rdata 里所有形如描述项的 0x20 字节块（两个名字指针相同且指向可读字符串）"""
    for lo, hi in data:
        for a in range(lo & ~7, hi - 0x20, 8):
            p1, p2, A, B = struct.unpack_from('<QQQQ', img, a)
            if p1 != p2 or not (base + 0x1000 <= p1 < base + len(img)): continue
            n = cstr(p1)
            if not n: continue
            yield a, n, A & 0xffff, A >> 16 & 0xffff, A >> 32 & 0xffff, A >> 48, B


def show(e):
    a, n, off, size, ty, sid, B = e
    tgt = f' → 结构{B >> 16 & 0xffff:#05x}' if ty in (0x1c, 0x24, 0x2c, 0x28) else (f' 枚举{B >> 32 & 0xffff:#x}' if ty == 0x204 else '')
    return f'{a:#x} 结构{sid:#05x} +{off:#06x} 大小{size:3d} 类型{ty:#06x}{tgt}  {n}'


ents = list(entries())
if args[1] == '--struct':
    for want in (int(x, 16) for x in args[2:]):
        print(f'=== 结构 {want:#x}')
        for e in sorted((e for e in ents if e[5] == want), key=lambda e: (e[2], e[0])): print(' ', show(e))
elif args[1] == '--ptrto':
    for want in (int(x, 16) for x in args[2:]):
        print(f'=== 指向结构 {want:#x} 的字段')
        for e in ents:
            if e[4] in (0x1c, 0x24, 0x2c, 0x28) and (e[6] >> 16 & 0xffff) == want: print(' ', show(e))
else:
    for name in args[1:]:
        print(f'=== 叫 {name} 的字段')
        for e in ents:
            if e[1] == name or e[1].endswith('::' + name): print(' ', show(e))
