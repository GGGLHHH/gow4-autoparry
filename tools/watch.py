"""只读：每 100 ms 读一组全局，值变化时打印。用法: watch.py <秒数> <rva>:<类型>:<名字> ..."""
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peek import Game, FMT
g = Game(); dur = float(sys.argv[1]); specs = [a.split(':') for a in sys.argv[2:]]
last = {}; t0 = time.time()
print(f'pid={g.pid} 开始监视 {dur:.0f}s', flush=True)
while time.time() - t0 < dur:
    for rva, t, nm in specs:
        b = g.read(g.base + int(rva, 16), struct.calcsize(FMT[t]))
        v = struct.unpack(FMT[t], b)[0] if b else None
        if last.get(nm, 'x') != v:
            print(f'{time.time() - t0:7.1f}s  {nm} = {v}', flush=True); last[nm] = v
    time.sleep(0.1)
