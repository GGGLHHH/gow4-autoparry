"""只读：按记录类型名列出招式静态数据的时间轴记录（kT* 名字来自 exe 里的枚举表 0x1123a18 一带）。
用法:
  movedump2.py <招式地址十六进制>[:时长秒]...        直接导出
  movedump2.py --l1 <gow4_qte.log> [起始行号]         从 QTE 记录里取 L1 跳转（事件 5）指向的招式再导出
招式 m：+0x20 记录指针数组、+0x28 个数、+0x40 哈希；记录 = u32 类型(低 8 位), f16 开始, f16 结束（占整招比例）"""
import re, struct, sys
from golist import g

KT = {0: 'None', 1: 'Container', 2: 'SetFlags', 3: 'Synch', 4: 'Group', 5: 'SetPartMask', 6: 'DampingAdjust', 7: 'LightAdjust',
      0x10: 'Death', 0x11: 'Sound', 0x12: 'SoundOnEnemy', 0x13: 'SoundWindow', 0x1c: 'Slowdown', 0x1d: 'Script', 0x1e: 'TimeWarp',
      0x1f: 'Context', 0x20: 'Block', 0x21: 'NoBlock', 0x22: 'HitPause', 0x23: 'BlockPause', 0x24: 'LuaHook', 0x25: 'LuaCameraHook',
      0x26: 'HeadTrack', 0x27: 'FootEffect', 0x28: 'CustomEffect', 0x2a: 'CameraStart', 0x2c: 'AlignToCollNormal', 0x2d: 'EffectTrigger',
      0x2e: 'StartEffect', 0x2f: 'StopEffect', 0x30: 'SetMix', 0x31: 'AnimLayer', 0x33: 'InhibitDTree', 0x34: 'CameraFlags',
      0x35: 'Deflect', 0x36: 'WwiseRTPC', 0x37: 'WwiseState', 0x38: 'WwiseSwitch', 0x39: 'CameraRecenter', 0x45: 'MoveTelemetry'}
SHOW = {0x20, 0x21, 0x23, 0x35, 0x2, 0x1c, 0x22, 0x1f}   # 这些类型把原始字节也打出来


def rd(a, n):
    try:
        return g.read(a, n)
    except Exception:
        return None


def q(a):
    b = rd(a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def f16(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]


def dump(m, dur=None, label=''):
    hdr = rd(m, 0x48)
    if not hdr:
        print(f'== {m:#x} 读不到'); return
    arr, n, h = struct.unpack_from('<Q', hdr, 0x20)[0], struct.unpack_from('<I', hdr, 0x28)[0], struct.unpack_from('<Q', hdr, 0x40)[0]
    if n > 400:
        print(f'== {m:#x} 记录数 {n} 不像招式'); return
    print(f'== 招式 {m:#x} 哈希 {h:016X} 时长 {dur if dur else "?"} 记录 {n} {label}')
    for i in range(n):
        e = q(arr + i * 8); b = rd(e, 0x60)
        if not b: continue
        t, s, en = struct.unpack_from('<IHH', b, 0)
        ty = t & 0xff; name = KT.get(ty, f'{ty:#x}')
        a, z = f16(s), f16(en)
        span = f'{a * dur:6.3f}–{z * dur:6.3f}s' if dur else f'{a:5.3f}–{z:5.3f}'
        line = f'  {i:2d} {name:<14} 类型{t:#06x} {span}'
        if ty == 0x1d:
            th, par = struct.unpack_from('<QQ', b, 0x10)
            line += f'  动作{th:016x} 参数{par:016x}'
        elif ty in SHOW:
            line += '  ' + ' '.join(f'{x:016x}' for x in struct.unpack_from('<10Q', b, 8))
        print(line)


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--l1':
        start = int(args[2]) if len(args) > 2 else 0
        seen = {}
        pat = re.compile(r'新提示 ([0-9A-F]+) 动作ID=0x005 类型=(\d+) .*?原始:(.*)$')
        for n, raw in enumerate(open(args[1], encoding='utf-8', errors='replace'), 1):
            if n < start: continue
            m = pat.search(raw)
            if not m: continue
            b = bytes(int(x, 16) for x in m[3].replace('|', ' ').split())
            if len(b) < 0x100: continue
            tgt = struct.unpack_from('<Q', b, 0)[0]
            seen.setdefault(tgt, []).append((int(m[2]), b[0xc], f16(struct.unpack_from('<H', b, 8)[0]), f16(struct.unpack_from('<H', b, 0xa)[0])))
        for tgt, lst in seen.items():
            kinds = sorted({(it, bt) for it, bt, _, _ in lst})
            dump(tgt, None, f'← L1 跳转 {len(lst)} 条（输入类型, 跳转类型）{kinds}')
    else:
        for a in args:
            p = a.split(':'); dump(int(p[0], 16), float(p[1]) if len(p) > 1 else None)
