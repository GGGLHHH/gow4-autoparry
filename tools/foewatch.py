"""只读：盯着场上的敌人若干秒，把它们用过的每个招式连同攻击段列表记下来（每招只打印一次）。
生物 = GO+0x220；招式系统 ms = 生物+0x3a70；当前招式 ms+0x118、已进行 ms+0xd4、时长 ms+0xe4、速率 ms+0xe8。
攻击段见 movecoll.py。用法: foewatch.py [秒数=20] [名字前缀=全部敌人]"""
import struct, sys, time
from golist import g, q, objects

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
want = sys.argv[2] if len(sys.argv) > 2 else ''
LEVEL, SHIELD = 0x3F8000, 0x278000


def rd(a, n):
    try:
        return g.read(a, n)
    except Exception:
        return None


def name(go):
    s = (rd(q(go + 0x38), 32) or b'').split(b'\0')[0]
    return s.decode('ascii', 'replace') if s and all(32 <= c < 127 for c in s) else ''


def f16(b, o):
    return struct.unpack_from('<e', b, o)[0]


def foes():
    out, seen = [], set()
    for _, _, go in objects():
        n = name(go)
        if not n.startswith('go') or n.startswith(('gohero', 'goson', 'gopickup')) or (want and not n.startswith(want)) or go in seen:
            continue
        seen.add(go)
        c = q(go + 0x220)
        ms = q(c + 0x3a70) if c else 0
        if c and ms and q(c + 8) == go and q(ms + 0x10c8) == c:
            out.append((n, c, ms))
    return out


fs = foes()
print(f'场上敌人 {len(fs)} 个：' + ', '.join(f'{n}@{c:#x}' for n, c, _ in fs))
seen, t0, last = set(), time.time(), 0.0
while fs and time.time() - t0 < secs:
    for n, c, ms in fs:
        b = rd(ms + 0xd4, 0x18)
        m = q(ms + 0x118)
        if not b or not m or (c, m) in seen:
            continue
        seen.add((c, m))
        el, dur, rate = struct.unpack_from('<f', b, 0)[0], struct.unpack_from('<f', b, 0x10)[0], struct.unpack_from('<f', b, 0x14)[0]
        h = rd(m, 0x70)
        if not h:
            continue
        colls, nc = struct.unpack_from('<QI', h, 0x10)
        print(f'{time.strftime("%H:%M:%S")} {n} 招式 {m:#x} 哈希 {struct.unpack_from("<Q", h, 0x40)[0]:016X} 时长 {dur:.3f} s 速率 {rate:.2f} 已进行 {el:.3f} 攻击段 {nc}')
        for i in range(min(nc, 16)):
            p = q(colls + i * 8)
            cb = rd(p, 0x90)
            if not cb:
                continue
            on, off = f16(cb, 0), f16(cb, 2)
            part, fl = struct.unpack_from('<II', cb, 0x40)
            hf = struct.unpack_from('<Q', cb, 0x48)[0]
            red = (fl & 4) or ((hf >> 48) & 4)
            nosh = (hf & LEVEL) and not (hf & SHIELD)
            print(f'      段{i}: 出伤 {on:.3f}–{off:.3f}（×时长 = {on * dur:.3f}–{off * dur:.3f} s） 伤害 {f16(cb, 8):g} Flags {fl:#x} HitFlags {hf:016X}'
                  f'{"  ← 不可格挡" if red else ("  ← 盾接不了" if nosh else "")}')
    time.sleep(0.008)
    if time.time() - last > 5:
        last = time.time(); fs = foes() or fs
print(f'共记到 {len(seen)} 个招式')
