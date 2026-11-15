"""反汇编 GoW.exe 指定 RVA（只读）。用法: dis.py <exe> <rva> [条数]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
rva = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
data = pe.get_data(rva, n * 15)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i, ins in zip(range(n), md.disasm(data, pe.OPTIONAL_HEADER.ImageBase + rva)):
    print(f"{ins.address - pe.OPTIONAL_HEADER.ImageBase:#09x}  {ins.bytes.hex():24} {ins.mnemonic} {ins.op_str}")
