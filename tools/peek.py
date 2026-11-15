"""只读：读运行中 GoW.exe 的模块基址 + 指定 RVA 的值。用法: peek.py <rva>:<类型>[:名字] ...  类型 u8/u16/i16/u32/f32/u64"""
import ctypes, ctypes.wintypes as W, struct, sys
k32 = ctypes.WinDLL('kernel32', use_last_error=True); psapi = ctypes.WinDLL('psapi')
def find_pid(name='GoW.exe'):
    import subprocess
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH', '/FI', f'IMAGENAME eq {name}'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.strip('"').split('","')
        if p[0].lower() == name.lower(): return int(p[1])
    raise SystemExit('GoW.exe 没在运行')
class Game:
    def __init__(self):
        self.pid = find_pid()
        self.h = k32.OpenProcess(0x0410, False, self.pid)  # QUERY_INFORMATION | VM_READ，不要写权限
        if not self.h: raise SystemExit(f'OpenProcess 失败 {ctypes.get_last_error()}')
        mods = (W.HMODULE * 1)(); need = W.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        self.base = mods[0]
    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)): return None
        return buf.raw
FMT = {'u8': '<B', 'u16': '<H', 'i16': '<h', 'u32': '<I', 'i32': '<i', 'f32': '<f', 'u64': '<Q'}
if __name__ == '__main__':
    g = Game(); print(f'pid={g.pid} base={g.base:#x}')
    for a in sys.argv[1:]:
        rva, t, *nm = a.split(':'); b = g.read(g.base + int(rva, 16), struct.calcsize(FMT[t]))
        print(f'{(nm or [""])[0]:24} +{rva}  {t} = {struct.unpack(FMT[t], b)[0] if b else "读失败"}')
