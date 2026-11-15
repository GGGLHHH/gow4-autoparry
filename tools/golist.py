"""只读遍历游戏对象：M=*(base+0x123b5d8)；两组区域表 M+0x10 / M+0x1918（个数在 +0x1900）；
每个区域 E 的链表头 *(E+0x50)，节点 next=*node，对象 GO=node-0x10。"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peek import Game
g = Game()
q = lambda a: struct.unpack('<Q', g.read(a, 8) or b'\0' * 8)[0]
u32 = lambda a: struct.unpack('<I', g.read(a, 4) or b'\0' * 4)[0]
M = q(g.base + 0x123b5d8)
def objects():
    for bank in (M + 0x10, M + 0x1918):
        n = u32(bank + 0x1900)
        for i in range(min(n, 800)):
            E = q(bank + i * 8)
            if not E: continue
            node, guard = q(E + 0x50), 0
            while node and guard < 100000:
                yield bank - M, i, node - 0x10
                node = q(node); guard += 1
if __name__ == '__main__':
    print(f'M={M:#x}')
    gos = list(objects())
    print('对象数', len(gos))
    ident = re.compile(rb'[A-Za-z_][A-Za-z0-9_\-./]{3,80}\0')
    for bank, i, go in gos[:int(sys.argv[1]) if len(sys.argv) > 1 else 6]:
        raw = g.read(go, 0x300) or b''
        print(f'\n## bank+{bank:#x}[{i}] GO={go:#x} id={u32(go + 0x280)}')
        # 对象里的指针如果指向可读 ASCII 名字就打出来
        for off in range(0, len(raw) - 8, 8):
            p = struct.unpack_from('<Q', raw, off)[0]
            if 0x10000 < p < 0x7fffffffffff:
                s = g.read(p, 64)
                if s and (m := ident.match(s)):
                    print(f'  +{off:#05x} -> "{m.group()[:-1].decode()}"')
        for m in ident.finditer(raw):
            print(f'  内联 +{m.start():#05x} "{m.group()[:-1].decode()}"')
