"""统计自动弹反日志：每次「吞掉并按下 L1」之后的结局，按版本、按敌人分组。用法: parrystat.py <日志> [起始版本，如 6.11]"""
import re, sys, collections
path = sys.argv[1]; vmin = tuple(int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else '6.8').split('.'))
T = re.compile(r'^(\d\d):(\d\d):(\d\d)\.(\d{3}) (.*)$')
def ms(m): return ((int(m[1]) * 60 + int(m[2])) * 60 + int(m[3])) * 1000 + int(m[4])
ver = None; on = False
attempts = []   # dict(t, who, ver, events=[(dt, kind, extra)])
cur = {}        # who -> 最近一次尝试
judge = collections.Counter()
for raw in open(path, encoding='utf-8', errors='replace'):
    m = T.match(raw.rstrip('\n'))
    if not m: continue
    t = ms(m); s = m[5]
    b = re.search(r'==== 自动弹反实验 v(\d+)\.(\d+)', s)
    if b:
        ver = (int(b[1]), int(b[2])); on = ver >= vmin; cur = {}; continue
    if not on: continue
    c = re.match(r'(\S+) 碰到奎托斯：(.*)$', s)
    if c:
        who, rest = c[1], c[2]
        kind = next((k for k in ('吞掉并按下 L1', '继续吞', '放行（已在弹反窗口）', '放行（吞超时）', '放行（冷却中）') if rest.startswith(k)), rest[:14])
        extra = rest[len(kind):]
        if kind.startswith('吞掉并按下 L1'):
            a = dict(t=t, who=who, ver=ver, ev=[], extra=extra); attempts.append(a); cur[who] = a
        elif who in cur and t - cur[who]['t'] < 2500:
            cur[who]['ev'].append((t - cur[who]['t'], kind, extra))
        continue
    p = re.search(r'弹反成功：奎托斯震到 (\S+?)（', s)
    if p and p[1] in cur and t - cur[p[1]]['t'] < 2500:
        cur[p[1]]['ev'].append((t - cur[p[1]]['t'], '★', '')); continue
    j = re.search(r'格挡判定 返回=(\d) 输出=(\d) .*?匹配防御体=[0-9A-Fa-f]+[（(]半径 ([\d.]+)', s)
    if j:
        # 归到最近 2.5 s 内有尝试的那个敌人身上（日志这一行不带名字，取最近的一次尝试）
        best = max((a for a in cur.values() if 0 <= t - a['t'] < 2500), key=lambda a: a['t'], default=None)
        if best is not None:
            best['ev'].append((t - best['t'], f'判定{j[1]}{j[2]}r{float(j[3]):.0f}', ''))
        judge[(j[1], j[2], j[3])] += 1
def outcome(a):
    kinds = [k for _, k, _ in a['ev']]
    if '★' in kinds: return '弹反成功'
    rel = [d for d, k, _ in a['ev'] if k.startswith('放行（已在弹反窗口')]
    tout = any(k.startswith('放行（吞超时') for k in kinds)
    js = [k for k in kinds if k.startswith('判定')]
    if rel:
        if any(k.startswith('判定11') for k in js): return '放行后判成弹反但没震到敌人'
        if any(k.startswith('判定10') for k in js): return '放行后只算普通格挡'
        if any(k.startswith('判定0') for k in js): return '放行后挨打'
        return '放行后没有判定记录'
    if tout: return '吞超时'
    if any(k.startswith('继续吞') for k in kinds): return '一直吞到这一刀结束（没弹也没挨打）'
    return '只碰了一下（吞掉，没弹也没挨打）'
by = collections.defaultdict(collections.Counter); tot = collections.Counter(); rels = collections.defaultdict(list)
for a in attempts:
    o = outcome(a); name = re.sub(r'\d+$', '', a['who']); by[name][o] += 1; tot[o] += 1
    for d, k, _ in a['ev']:
        if k.startswith('放行（已在弹反窗口'): rels[name].append(d); break
print(f'版本 ≥ v{vmin[0]}.{vmin[1]}：尝试 {len(attempts)} 次');
for o, n in tot.most_common(): print(f'  {o}: {n}（{n * 100 // max(1, len(attempts))}%）')
print('--- 按敌人')
for name, c in sorted(by.items(), key=lambda kv: -sum(kv[1].values())):
    r = sorted(rels[name]); med = r[len(r) // 2] if r else -1
    print(f'{name}: 共 {sum(c.values())}，' + '，'.join(f'{o} {n}' for o, n in c.most_common()) + f'；放行时刻中位 {med} ms')
print('--- 失败样例（每类前 3 条）')
seen = collections.Counter()
for a in attempts:
    o = outcome(a)
    if o == '弹反成功' or seen[o] >= int(sys.argv[3] if len(sys.argv) > 3 else 3): continue
    seen[o] += 1
    h, r = divmod(a['t'], 3600000); mi, r = divmod(r, 60000)
    print(f"v{a['ver'][0]}.{a['ver'][1]} {h:02}:{mi:02}:{r / 1000:06.3f} {a['who']} [{o}] " + ' '.join(f'{d}:{k}' for d, k, _ in a['ev'][:14] if not k.startswith('继续吞')) + f" | {a['extra'][:90]} (继续吞 {sum(1 for _, k, _ in a['ev'] if k.startswith('继续吞'))} 次)")
