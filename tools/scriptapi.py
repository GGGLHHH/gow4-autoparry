"""扫 .data 里所有 {名字指针, 函数指针} 形式的脚本接口注册项（步长 0x18），按关键词过滤（只读）。
用法: scriptapi.py <exe> [正则]"""
import re, struct, sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
t0, t1 = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
data = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.data')
d0, d1 = data.VirtualAddress, data.VirtualAddress + data.SizeOfRawData
rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else '.', re.I)
ident = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{2,60}\0')
out = []
for r in range(d0, d1 - 16, 8):
    n, f = struct.unpack_from('<QQ', img, r)
    if not (t0 <= f < t1) or not (base <= n < base + len(img)): continue
    m = ident.match(img, n - base)
    if not m: continue
    name = m.group()[:-1].decode()
    if rx.search(name): out.append((r, name, f - base))
for r, name, f in out: print(f'{r:#x}  {name:48} fn={f:#x}')
print(len(out), 'entries', file=sys.stderr)
