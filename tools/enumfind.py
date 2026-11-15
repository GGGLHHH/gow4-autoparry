"""在 {名字指针, 名字指针, 值} 枚举表里按值或名字正则找表项，并打印它所在的整张表（前后连续的表项）。
用法: enumfind.py <exe> v:<十六进制值> | n:<名字正则>  [前后各多少项，默认 40]"""
import re, struct, sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
key = sys.argv[2]; span = int(sys.argv[3]) if len(sys.argv) > 3 else 40
def sat(q):
    r = q - base
    if not (0x1000 <= r < len(img)): return None
    e = img.find(b'\0', r, r + 96)
    s = img[r:e] if e > r else b''
    return s.decode() if len(s) >= 2 and all(32 <= c < 127 for c in s) else None
def entry(a):
    if a < 0 or a + 24 > len(img): return None
    p1, p2, v = struct.unpack_from('<QQQ', img, a)
    n1, n2 = sat(p1), sat(p2)
    return (n1, n2, v) if n1 and n2 and n1.startswith('k') and (n1[1:] == n2 or n1 == n2) else None
hits = []
for s in pe.sections:
    if s.Name.rstrip(b'\0') not in (b'.data', b'.rdata'): continue
    for a in range(s.VirtualAddress & ~7, s.VirtualAddress + s.Misc_VirtualSize - 24, 8):
        e = entry(a)
        if not e: continue
        if key.startswith('v:') and e[2] == int(key[2:], 16) or key.startswith('n:') and re.search(key[2:], e[0]):
            hits.append(a)
done = set()
for h in hits:
    lo = h
    while entry(lo - 24) and (h - lo) // 24 < span: lo -= 24
    hi = h
    while entry(hi + 24) and (hi - h) // 24 < span: hi += 24
    if (lo, hi) in done: continue
    done.add((lo, hi))
    print(f'--- 表 {lo:#x}..{hi:#x}（命中 {h:#x}）')
    for a in range(lo, hi + 24, 24):
        e = entry(a); print(f'  {e[0]:<40} {e[2]:#x}' + ('   <==' if a == h else ''))
