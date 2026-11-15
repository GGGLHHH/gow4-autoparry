"""只读：列出一个招式静态数据里的时间轴记录。招式 m：+0x20 记录指针数组、+0x28 个数；
记录 = u32 类型, f16 开始, f16 结束（占整招比例）, ..., +0x10 动作类型哈希, +0x18 参数"""
import struct, sys
from golist import g
q=lambda a: struct.unpack('<Q',g.read(a,8))[0]
def f16(h): return struct.unpack('<e',struct.pack('<H',h))[0]
def dump(m,dur,label):
    arr=q(m+0x20); n=q(m+0x28)&0xffffffff
    print(f'== {label} move={m:#x} 时长 {dur:.3f}s 记录 {n}')
    for i in range(n):
        e=q(arr+i*8); b=g.read(e,0x20)
        t,s,en=struct.unpack_from('<IHH',b,0)
        th,par=struct.unpack_from('<QQ',b,0x10)
        if t!=0x1d: continue
        print(f'  {i:2d} 类型{t:#x} {f16(s)*dur:6.3f}–{f16(en)*dur:6.3f}s  动作{th:016x} 参数{par:016x} ({struct.unpack_from("<f",b,0x18)[0]:.3g})')
if __name__=='__main__':
    for a in sys.argv[1:]:
        m,dur,lab=a.split(':'); dump(int(m,16),float(dur),lab)
