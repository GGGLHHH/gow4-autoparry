"""只读：列出奎托斯附近的可采集物（世界坐标 + 已拿/未拿）。
用法：collectibles.py [半径米=60] [--all 连已拿的也列]
判定依据（2026-11-14 实测 4 个样本）：物品对象 +0x278 的 0x00200000 位清零 = 已拿走。
单位：游戏坐标就是米（相机在奎托斯身后约 2.8，正好是越肩视角的距离）。"""
import os, sys, re, math, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gow import objects, name, world, player, g

KINDS = [  # (正则, 中文类别) —— 只认「真正被拿走的那个物品对象」，不认容器/脚本/特效
    (r'^gochestpouchlegendary|^gopouchlegendary', '传说宝箱/袋'),
    (r'^gochestpouchrare', '稀有宝箱'),
    (r'^gochestpouch', '宝箱'),
    (r'^gopouchsilver$|^gohacksilver', '银币袋'),
    (r'^goartifacttoy\d|^goartifact\d', '古物'),
    (r'healthrune|^gokeyrune\d', '符文'),
    (r'^golootbag$', '战利品袋'),
    (r'^gobelltargetfront$', '符文铃铛'),  # 诺恩宝箱的限时铃铛机关（不是拿走的物品，已拿位对它没意义）
    (r'^gospinner$', '符文转盘'),  # 诺恩宝箱的转盘机关；这里不做「属于哪个箱子」的层级判断，可能列出无关的同名对象
    (r'^gorunic\d*_break$', '符文封印'),
]
TAKEN_BIT = 0x00200000

def scan(radius_m=60.0, show_taken=False):
    _, P = player()
    out = []
    for _, _, go in objects():
        n = name(go)
        if n.endswith('_smsh'): continue  # 碎裂物件的变体
        kind = next((k for rx, k in KINDS if re.search(rx, n)), None)
        if not kind: continue
        w, _ = world(go)
        d = math.dist(w, P)
        if d > radius_m: continue
        flags = struct.unpack('<I', g.read(go + 0x278, 4))[0]
        taken = not (flags & TAKEN_BIT)
        if taken and not show_taken: continue
        dy = w[1] - P[1]
        out.append((d, kind, n, taken, dy, go))
    seen, uniq = set(), []  # 同一处同类别只留一条（同一个古物/符文常有 2~4 个对象叠在一起）
    for r in sorted(out):
        key = (r[1], round(r[0], 1))
        if key not in seen:
            seen.add(key); uniq.append(r)
    return P, uniq

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    P, rows = scan(float(args[0]) if args else 60.0, '--all' in sys.argv)
    print(f'奎托斯 ({P[0]:.1f}, {P[1]:.1f}, {P[2]:.1f})  共 {len(rows)} 个')
    for d, kind, n, taken, dy, go in rows:
        h = f'高{dy:+.1f}m' if abs(dy) > 1 else ''
        print(f'{d:6.1f}m  {kind:8} {"已拿" if taken else "    "}  {h:9} {n}')
