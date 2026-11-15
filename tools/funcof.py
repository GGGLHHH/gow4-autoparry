"""用 .pdata 找 RVA 所在函数的起止（只读）。用法: funcof.py <exe> <rva>..."""
import sys, struct, bisect, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image()
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
rf = [struct.unpack_from('<III', img, d.VirtualAddress + i) for i in range(0, d.Size, 12)]
rf.sort(); starts = [r[0] for r in rf]
def root(i):  # 跟随 chained unwind 找到主函数入口
    b, e, u = rf[i]
    while True:
        flags = img[u & ~1] >> 3 if not (u & 1) else 0
        if flags & 4:  # UNW_FLAG_CHAININFO
            cnt = img[(u & ~1) + 2]; off = (u & ~1) + 4 + ((cnt + 1) & ~1) * 2
            b2, e2, u2 = struct.unpack_from('<III', img, off); b, u = b2, u2
        else:
            return b
for a in sys.argv[2:]:
    r = int(a, 16); i = bisect.bisect_right(starts, r) - 1
    b, e, u = rf[i]
    print(f'{r:#x}: chunk {b:#x}-{e:#x}  function entry {root(i):#x}' if b <= r < e else f'{r:#x}: 不在任何 pdata 范围内')
