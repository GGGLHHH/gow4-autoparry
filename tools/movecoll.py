"""只读：打印招式的 CollisionList（这一招会打出去的每一段攻击）。
招式 Move（exe 字段表结构 0x3ca）：+0x00 BranchList、+0x10 CollisionList{指针, u32 个数}、+0x20 ActionList、
  +0x38 AnimName、+0x40 MoveName（哈希）、+0x50 PlaybackSpeed f16、+0x5a Duration f16、+0x5c Flags。
Collision（结构 0x12f，就是命中时碰撞上下文 +0x80 的「攻击定义」）：+0x00 On f16、+0x02 Off f16（占整招的比例）、
  +0x04 HitPause、+0x06 BlockPause、+0x08 Damage、+0x0a Stun（都是 f16）、+0x40 PartFlags、+0x44 Flags（4 = kFUnblockable）、
  +0x48 HitFlags（u64）、+0x78 HitJoint。
用法: movecoll.py <招式地址十六进制>..."""
import struct, sys
from golist import g

LEVEL, SHIELD = 0x3F8000, 0x278000


def rd(a, n):
    try:
        return g.read(a, n)
    except Exception:
        return None


def f16(b, o):
    return struct.unpack_from('<e', b, o)[0]


def dump(m):
    h = rd(m, 0x70)
    if not h:
        print(f'== {m:#x} 读不到'); return
    colls, n = struct.unpack_from('<QI', h, 0x10)
    name = struct.unpack_from('<Q', h, 0x40)[0]
    dur, speed = f16(h, 0x5a), f16(h, 0x50)
    print(f'== 招式 {m:#x} 哈希 {name:016X} 时长 {dur:.3f} s 速度 {speed:.2f} 分支 {struct.unpack_from("<I", h, 8)[0]} 攻击段 {n} 动作 {struct.unpack_from("<I", h, 0x28)[0]}')
    if n > 64:
        print('   攻击段个数不像话，跳过'); return
    for i in range(n):
        p = struct.unpack('<Q', rd(colls + i * 8, 8) or b'\0' * 8)[0]
        c = rd(p, 0x90)
        if not c:
            print(f'  {i}: 指针 {p:#x} 读不到'); continue
        on, off, dmg, stun = f16(c, 0), f16(c, 2), f16(c, 8), f16(c, 0xa)
        part, fl = struct.unpack_from('<II', c, 0x40)
        hf = struct.unpack_from('<Q', c, 0x48)[0]
        red = (fl & 4) or ((hf >> 48) & 4)
        nosh = (hf & LEVEL) and not (hf & SHIELD)
        print(f'  {i}: {p:#x} 出伤 {on:.3f}–{off:.3f}（{on * dur:.3f}–{off * dur:.3f} s） 伤害 {dmg:g} 眩晕 {stun:g} Flags {fl:#x} HitFlags {hf:016X}'
              f'{"  ← 不可格挡" if red else ("  ← 盾接不了" if nosh else "")}')


for a in sys.argv[1:]:
    dump(int(a, 16))
