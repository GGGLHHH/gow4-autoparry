"""只读：列出场上每个生物的「生物定义」里和格挡/弹反有关的字段。
生物定义 = *(生物+0x4c30)（exe 字段表里的结构 0x344）：+0x448 ParryReaction（被弹反时播的招式）、
+0x450 ParryMask、+0x458 BlockMask（都是命中标志掩码，枚举 kHit*）。
生物从游戏对象表里找：对象 GO 的前 0x400 字节里，指向「+8 处又指回 GO」的那个指针就是生物。
用法: defmask.py [名字前缀过滤]"""
import struct, sys
from golist import g, q, objects

want = sys.argv[1] if len(sys.argv) > 1 else ''


def name(go):
    s = g.read(q(go + 0x38), 32) or b''
    s = s.split(b'\0')[0]
    return s.decode('ascii', 'replace') if s and all(32 <= c < 127 for c in s) else ''


seen = set()
for _, _, go in objects():
    n = name(go)
    if not n.startswith('go') or (want and not n.startswith(want)) or go in seen:
        continue
    seen.add(go)
    raw = g.read(go, 0x400) or b''
    for off in range(0, len(raw) - 8, 8):
        c = struct.unpack_from('<Q', raw, off)[0]
        if not (0x10000 < c < 0x7fffffffffff) or q(c + 8) != go:
            continue
        ms = q(c + 0x3a70)
        if not ms or q(ms + 0x10c8) != c:      # 招式系统要指回生物，否则不是生物
            continue
        d = q(c + 0x4c30)
        if not d:
            continue
        b = g.read(d + 0x440, 0x28) or b'\0' * 0x28
        boat, react, pmask, bmask, ray = struct.unpack('<5Q', b)
        rh = q(react + 0x40) if react else 0
        print(f'{n:<22} 生物 {c:#x} (GO+{off:#x}) 定义 {d:#x} 定义标志 {q(d) & 0xffffffff:#x}  ParryReaction={react:#x}'
              f'{f"(招式 {rh:016X})" if react else ""}  ParryMask={pmask:016X}  BlockMask={bmask:016X}')
        break
