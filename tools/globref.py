"""找代码段里 rip 相对访问某个全局 RVA 的指令（只读）。用法: globref.py <exe> <rva> [<rva>...]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'); code = text.get_data(); va = text.VirtualAddress
targets = [int(x, 16) for x in sys.argv[2:]]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
import struct
for t in targets:
    print(f'## {t:#x}')
    # 粗筛：任意位置的 disp32 让 下一条指令地址 + disp == t（指令长 5..10，disp 位于末尾前 0..4 字节）
    for i in range(len(code) - 4):
        d = struct.unpack_from('<i', code, i)[0]
        for tail in (0, 1, 2, 4):
            end = va + i + 4 + tail
            if end + d == t:
                for start in range(max(0, i - 6), i):
                    ins = next(md.disasm(code[start:start + 15], base + va + start), None)
                    if ins and ins.size == i + 4 + tail - start and 'rip' in ins.op_str:
                        print(f'  {va + start:#x}  {ins.mnemonic} {ins.op_str}')
                        break
