"""只读：打印奎托斯防御组件里的防御组和防御体。用法：defset.py <奎托斯生物指针hex> [次数] [间隔秒]"""
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peek import Game
g = Game(); hero = int(sys.argv[1], 16)
q = lambda a: struct.unpack('<Q', g.read(a, 8))[0]
def snap():
    D = q(hero + 0x3a70); n = struct.unpack('<i', g.read(D + 0x48, 4))[0]
    out = []
    for i in range(n):
        S = q(D + 8 + i * 8); c = struct.unpack('<i', g.read(S + 0x80, 4))[0]
        vols = [q(S + k * 8) for k in range(max(0, min(c, 16)))]
        out.append((S, c, [(v, struct.unpack('<f', g.read(v + 0x40, 4))[0]) for v in vols]))
    return n, out
for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 1):
    n, out = snap()
    print(time.strftime('%H:%M:%S'), f'防御组 {n} 个：', '; '.join(f'{S:#x} 体数{c} ' + ','.join(f'{v:#x}(r={r:g})' for v, r in vs) for S, c, vs in out))
    if len(sys.argv) > 3: time.sleep(float(sys.argv[3]))
