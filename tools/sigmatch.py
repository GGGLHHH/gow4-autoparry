"""把一份特征码表（JSON：symbols[].pattern + mask）拿到战神4 GoW.exe 的代码段里试匹配（只读）。"""
import json, re, struct, sys

def load_text(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3c)[0]; n = struct.unpack_from('<H', b, pe + 6)[0]
    osz = struct.unpack_from('<H', b, pe + 20)[0]; opt = pe + 24
    for i in range(n):
        s = opt + osz + 40 * i
        if b[s:s + 8].rstrip(b'\0') == b'.text':
            vs, va, rs, ra = struct.unpack_from('<IIII', b, s + 8)
            return b[ra:ra + rs], va
    raise SystemExit('no .text')

def to_regex(pattern, mask):
    raw = bytes.fromhex(pattern); out = b''
    for i, c in enumerate(mask):
        out += b'.' if c == '?' else re.escape(raw[i:i + 1])
    return re.compile(out, re.S)

text, va = load_text(sys.argv[1])
sigs = json.load(open(sys.argv[2], encoding='utf-8'))['symbols']
hits = 0
for s in sigs:
    rx = to_regex(s['pattern'], s['mask'])
    m = [x.start() for x, _ in zip(rx.finditer(text), range(3))]
    if m:
        hits += 1
        print(f"{s['name']:28} {s['kind']:8} len={len(s['mask']):3} hits={len(m)} rva={[hex(x + va - s.get('matchOffset', 0)) for x in m]}  {s.get('purpose','')[:60]}")
print(f'{hits}/{len(sigs)} signatures matched')
