"""打印字段描述表：每项 0x20 字节 = {名字指针, 名字指针, u64 A, u64 B}，A = 偏移(16) | 大小(16) | 类型(16) | 结构号(16)。
用法: fields.py <exe> <表内任意一项的RVA> [往前多少项=40] [往后多少项=40] [--same 只要同一结构号]"""
import struct, sys, pefile
args = sys.argv[1:]; same = '--same' in args
if same: args.remove('--same')
pe = pefile.PE(args[0], fast_load=True); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
at = int(args[1], 16); nb = int(args[2]) if len(args) > 2 else 40; na = int(args[3]) if len(args) > 3 else 40
def s(q):
    r = q - base
    if not (0x1000 <= r < len(img)): return None
    e = img.find(b'\0', r, r + 100); t = img[r:e] if e > r else b''
    return t.decode() if len(t) >= 2 and all(32 <= c < 127 for c in t) else None
sid0 = struct.unpack_from('<Q', img, at + 0x10)[0] >> 48
for a in range(at - nb * 0x20, at + (na + 1) * 0x20, 0x20):
    p1, p2, A, B = struct.unpack_from('<QQQQ', img, a)
    n = s(p1)
    if not n: continue
    off, size, ty, sid = A & 0xffff, A >> 16 & 0xffff, A >> 32 & 0xffff, A >> 48
    if same and sid != sid0: continue
    print(f'{a:#x} 结构{sid:#05x} +{off:#06x} 大小{size:3d} 类型{ty:#06x} B={B:016x}  {n}' + ('   <==' if a == at else ''))
