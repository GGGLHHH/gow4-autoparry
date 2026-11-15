"""找引用某字符串的地方（数据里的 8 字节指针 + 代码里 rip 相对 lea），只读。用法: xref.py <exe> <字符串>..."""
import sys, struct, pefile, re
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
for name in sys.argv[2:]:
    for m in re.finditer(re.escape(name.encode()) + b'\0', img):
        if m.start() and img[m.start() - 1] != 0: continue
        srva = m.start(); print(f'## "{name}" @ {srva:#x}')
        for p in re.finditer(re.escape(struct.pack('<Q', base + srva)), img):
            r = p.start(); nxt = struct.unpack_from('<Q', img, r + 8)[0]
            print(f'  ptr @ {r:#x}  next qword {nxt:#x} (rva {nxt - base:#x})')
        for i in range(t0, t1 - 7):
            if img[i] in (0x48, 0x4C) and img[i + 1] == 0x8D and (img[i + 2] & 0xC7) == 0x05:
                if i + 7 + struct.unpack_from('<i', img, i + 3)[0] == srva:
                    print(f'  lea @ {i:#x}')
