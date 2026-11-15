"""只读：对象遍历 + 名字 + 世界坐标（考虑父节点缓存矩阵）。"""
import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from golist import g, objects, q
def name(go):
    p = q(go + 0x38); s = g.read(p, 80) if p else None
    return s.split(b'\0')[0].decode('latin1') if s else '?'
def world(go):
    b = g.read(go + 0x60, 0xF0)  # 本地矩阵 0x60..0x9f，父矩阵缓存 0xF0..0x12f，父类型 0x148
    if not b: return None
    L = struct.unpack_from('<16f', b, 0); P = struct.unpack_from('<16f', b, 0x90)
    kind = struct.unpack_from('<i', b, 0xE8)[0]
    lx, ly, lz = L[12], L[13], L[14]
    if kind in (2, 3, 4) and any(P):
        return tuple(lx * P[c] + ly * P[4 + c] + lz * P[8 + c] + P[12 + c] for c in range(3)), kind
    return (lx, ly, lz), kind
def player():
    for _, _, go in objects():
        if name(go) == 'goheroa00':
            w, _ = world(go)
            if any(w): return go, w
