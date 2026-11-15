// 战神4 自动弹反（实验 v3：吞掉第一下 + 替玩家按 L1）。
// v2（命中结算时换弹反体，源码留在 autoparry_v2_swap.cpp.txt）证明：结算结果和自然弹反一模一样也不出弹反动作，
// 弹反动作要奎托斯的动作状态机正处在「刚按 L1」的弹反窗口里才会接。所以 v3：
//   1. hook 碰撞→命中入口 0x8c9e30：敌人这一刀第一次碰到奎托斯、而他不在弹反窗口时，先不让它进结算（原函数本来就有
//      直接 return 的分支），同时开始「按住 L1」；
//   2. hook 按键条件判定 0x87ac70：按住期间，动作ID 0x05（L1）的类型 1/7 条件一律返回成立 —— 等于游戏内部按下 L1；
//   3. 奎托斯的防御组里出现半径 >0 的弹反体（= 进了弹反窗口）后，这一刀再碰到就放行 → 走游戏自己的弹反流程。
//   吞的时间有上限，超时就放行，不会变成无敌。
// 另外 hook 格挡判定 0x79f0c0 只记录结果。日志 %TEMP%\gow4_autoparry.log；卸载：gow4_inject unload gow4_autoparry。
// 注意：0x87ac70 和 QTE 探针 gow4_probe.dll 是同一个函数，要先卸探针。
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <share.h>
#include <unordered_map>
#include <unordered_set>

namespace {

HMODULE g_self = nullptr;
FILE* g_log = nullptr;
uintptr_t g_exe = 0;
std::atomic<int> g_inflight{0};

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

bool SafeRead(uintptr_t src, void* dst, size_t n)
{
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> T Rd(uintptr_t a)
{
    T v{};
    SafeRead(a, &v, sizeof(T));
    return v;
}

// ---- inline hook：前 8 字节原子替换为 E9 rel32 + NOP 填充，搬走的字节放跳板 ----
struct Hook {
    const char* name;
    uintptr_t rva;
    const uint8_t* expect;
    size_t expectLen;
    size_t stolen; // 5..8，必须是完整指令边界、无 rip 相对寻址
    void* detour;
    uint8_t* target = nullptr;
    uint8_t* tramp = nullptr;
    uint64_t original = 0;
    bool installed = false;
};

uint8_t* AllocNear(uint8_t* near_)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(near_) & ~(step - 1);
    for (uintptr_t d = step; d < 0x7FF00000; d += step)
        for (uintptr_t cand : {base - d, base + d})
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(cand), 4096, MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
    return nullptr;
}

void WriteAbsJmp(uint8_t* at, const void* to)
{
    at[0] = 0xFF;
    at[1] = 0x25;
    *reinterpret_cast<uint32_t*>(at + 2) = 0;
    *reinterpret_cast<const void**>(at + 6) = to;
}

bool SwapQword(uint8_t* target, uint64_t expect, uint64_t value)
{
    DWORD old;
    if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old))
        return false;
    const bool ok = static_cast<uint64_t>(_InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(target),
                        static_cast<long long>(value), static_cast<long long>(expect))) == expect;
    VirtualProtect(target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    return ok;
}

bool Install(Hook& h)
{
    h.target = reinterpret_cast<uint8_t*>(g_exe + h.rva);
    if ((reinterpret_cast<uintptr_t>(h.target) & 7) != 0 || memcmp(h.target, h.expect, h.expectLen) != 0) {
        Log("[%s] 目标字节不匹配或没对齐，不装", h.name);
        return false;
    }
    h.tramp = AllocNear(h.target);
    if (!h.tramp) {
        Log("[%s] 找不到跳板内存", h.name);
        return false;
    }
    memcpy(h.tramp, h.target, h.stolen);
    WriteAbsJmp(h.tramp + h.stolen, h.target + h.stolen);
    uint8_t* relay = h.tramp + 0x40;
    WriteAbsJmp(relay, h.detour);
    FlushInstructionCache(GetCurrentProcess(), h.tramp, 0x60);
    h.original = *reinterpret_cast<uint64_t*>(h.target);
    uint8_t patch[8];
    memcpy(patch, h.target, 8);
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(relay - (h.target + 5));
    for (size_t i = 5; i < h.stolen; ++i)
        patch[i] = 0x90;
    uint64_t value;
    memcpy(&value, patch, 8);
    if (!SwapQword(h.target, h.original, value)) {
        Log("[%s] 原子写入失败", h.name);
        return false;
    }
    h.installed = true;
    Log("[%s] hook 已装：目标=%p 跳板=%p", h.name, h.target, h.tramp);
    return true;
}

void Remove(Hook& h)
{
    if (!h.installed)
        return;
    const uint64_t cur = *reinterpret_cast<uint64_t*>(h.target);
    Log("[%s] %s", h.name, SwapQword(h.target, cur, h.original) ? "已恢复原字节" : "恢复原字节失败！");
}

// ---- 游戏结构 ----
// 生物 +8 → 游戏对象（+0x38 名字），+0x3a70 → 防御组件。
// 防御组件：+0x8 起防御组指针数组，+0x48 个数。防御组：+0x0 起 16 格防御体指针，+0x80 个数。防御体 +0x40 = 弹反范围（>0 即弹反体）。
// 碰撞→命中入口 0x8c9e30(a, b, c, d, e)：a+0x20 攻击者生物，b+0x20 受击者生物（见 docs/gow4-findings.md）。
constexpr int kSetSlots = 16;

// 时间参数（微秒）
constexpr int64_t kSwallowUs = 300'000; // 一刀最多吞这么久，超时放行
constexpr int64_t kHoldUs = 450'000;    // 按住 L1 多久
constexpr int64_t kRearmUs = kHoldUs + 30'000; // 上一次按住结束就能再按（留一帧松开）
constexpr int64_t kReleaseUs = 34'000; // 玩家自己按着 L1 时，先替他松开约两帧再按下，才有新的弹反窗口
std::atomic<int64_t> g_evadeTapUs{200'000};  // 闪避：× 按多久（实测你手按 150~200 ms；ini evade_ms）
// 翻滚 = 连按两下 ×：按 80 ms、松 80 ms、再按 120 ms，摇杆全程推着
std::atomic<bool> g_evadeRollNow{false};
bool g_seqRunning = false; // 只在后台线程读写
std::atomic<int64_t> g_manualCrossUs{0}; // 你自己最近一次按下 × 的时刻（换招记录用）
std::atomic<bool> g_evadeRoll{false}; // ini evade_roll：巨魔类预判闪避用翻滚（实验，默认关：v6.6 实测 5 次只翻出 1 次）
std::atomic<int64_t> g_rollTap1Us{120'000}, g_rollGapUs{280'000}, g_rollTap2Us{150'000}; // ini roll_tap1/roll_gap/roll_tap2（毫秒）
int64_t NowUs();
// 0 = 不在闪避，1 = × 按着，2 = × 松开但摇杆还推着
std::atomic<int64_t> g_evadeStart{0};
// 当前是第几下（1/2），给「按键判定先看到这一下、输入轮询才跟着写」用
int EvadePressIdx()
{
    const int64_t es = g_evadeStart.load(std::memory_order_relaxed);
    if (!es || !g_evadeRollNow.load(std::memory_order_relaxed))
        return 1;
    return NowUs() - es < g_rollTap1Us.load(std::memory_order_relaxed) ? 1 : 2;
}
// 按键判定钩子已经写过的那一下：(闪避起点 << 2) | 第几下
std::atomic<int64_t> g_condSawPress{0};
int EvadePhase()
{
    const int64_t es = g_evadeStart.load(std::memory_order_relaxed);
    if (!es)
        return 0;
    const int64_t t = NowUs() - es;
    if (!g_evadeRollNow.load(std::memory_order_relaxed))
        return t < g_evadeTapUs.load(std::memory_order_relaxed) ? 1 : 0;
    const int64_t a = g_rollTap1Us.load(std::memory_order_relaxed), b = a + g_rollGapUs.load(std::memory_order_relaxed);
    if (t < a)
        return 1;
    if (t < b)
        return 2;
    return t < b + g_rollTap2Us.load(std::memory_order_relaxed) ? 1 : 0;
}
// 闪避按「同一刀」吞：同一个攻击者的碰撞间隔 < 150 ms 算同一刀（巨魔下砸的冲击波能连着碰 1 秒多、追到 5 m 外），
// 闪过的那一刀整段吞掉，最长 2 s；隔开 150 ms 以上才算新的一刀
constexpr int64_t kEvadeGapUs = 150'000;
constexpr int64_t kEvadeMaxUs = 2'000'000;
constexpr int64_t kEvadeRearmUs = 400'000;
std::atomic<uintptr_t> g_evAttacker{0};
std::atomic<int64_t> g_evLast{0};
std::atomic<int64_t> g_predUntil{0};   // 预判闪避后，这个时刻之前同一攻击者的碰撞一律吞（兜底）
std::atomic<bool> g_predictOn{true};   // ini predict：按招式时间轴提前闪
std::atomic<int> g_predictLeadMs{350}; // ini predict_ms：离出伤还剩多少毫秒时按 ×
std::atomic<int> g_predictRollMs{750}; // ini predict_roll_ms：巨魔类用翻滚时的提前量
std::atomic<int> g_predictDistDm{60};  // ini predict_dist：攻击者在多少米内才预判（存分米）
std::atomic<bool> g_snapOn{false};
std::atomic<bool> g_predSwallow{true}; // ini pred_swallow：预判闪避后还碰到要不要吞（0 = 交给游戏判，做实验用）     // ini snap：招式系统逐帧快照（调试用）

int64_t NowUs()
{
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq * 1'000'000 + c.QuadPart % freq * 1'000'000 / freq;
}

std::atomic<uintptr_t> g_hero{0};
std::atomic<uint32_t> g_heroFlips{0};
std::atomic<int64_t> g_pressStart{0};
std::atomic<bool> g_l1Natural{false};   // 不强制时，游戏判定的 L1 按住（类型 7）结果 = 玩家自己是否按着
std::atomic<bool> g_releaseFirst{false};
std::atomic<uintptr_t> g_passAttacker{0}; // 弹反窗口里放行的那一刀的攻击者，用来认「震开」
std::atomic<int64_t> g_passUs{0};
std::atomic<uint32_t> g_parryOk{0}, g_parryTry{0};
std::atomic<uint32_t> g_forced{0}, g_condSeen{0};
std::atomic<bool> g_anyAngle{false}; // 实验过：清掉弹反体 +0x08 条件表也挡不住背后挨打，默认关
std::atomic<bool> g_parryOn{true};   // gow4_autoparry.ini [autoparry] parry=0/1，每秒重读
std::atomic<bool> g_anyZone{false};  // zone=0/1：弹反窗口里不限命中区域
std::atomic<bool> g_evadeOn{true};   // evade=0/1：背后来的刀改成自动闪避
std::atomic<int> g_backDeg{100};     // back_deg：方位角绝对值超过它算背后（0° 正前方，180° 正后方）
std::atomic<uint32_t> g_evadeForced{0};
std::atomic<float> g_evadeDirX{0.f}, g_evadeDirZ{0.f}; // 往哪闪（世界 x/z 单位向量）

// 游戏对象世界坐标：本地平移 GO+0x90；父类型 GO+0x148 为 2/3/4 时乘父矩阵缓存 GO+0xf0
bool WorldPos(uintptr_t go, float out[3])
{
    float L[16], P[16];
    if (!go || !SafeRead(go + 0x60, L, sizeof(L)) || !SafeRead(go + 0xf0, P, sizeof(P)))
        return false;
    const int kind = Rd<int>(go + 0x148);
    const bool parent = (kind == 2 || kind == 3 || kind == 4) && (P[0] != 0.f || P[5] != 0.f || P[10] != 0.f);
    for (int c = 0; c < 3; ++c)
        out[c] = parent ? L[12] * P[c] + L[13] * P[4 + c] + L[14] * P[8 + c] + P[12 + c] : L[12 + c];
    return true;
}

// 攻击者相对奎托斯的水平方位角（度，-180..180）。和游戏 0x723b60 一致：正前方 = 奎托斯矩阵 row2 的反方向；
// 攻击者在 row0 一侧时为负（游戏角度 = deg<0 ? 360+deg : deg）
bool Bearing(uintptr_t hero, uintptr_t attacker, float& deg, float& dist)
{
    const uintptr_t hg = Rd<uintptr_t>(hero + 8), ag = Rd<uintptr_t>(attacker + 8);
    float hp[3], ap[3], L[12];
    if (!WorldPos(hg, hp) || !WorldPos(ag, ap) || !SafeRead(hg + 0x60, L, sizeof(L)))
        return false;
    const float dx = ap[0] - hp[0], dz = ap[2] - hp[2];
    const float f = -(dx * L[8] + dz * L[10]), r = dx * L[0] + dz * L[2];
    deg = atan2f(-r, f) * 57.29578f;
    dist = sqrtf(dx * dx + dz * dz);
    return true;
}

// v6.20：奎托斯直接用游戏自己的玩家指针。脚本接口 FindPlayer(0x8267b0) 读 *(GoW+0x22e7d50) = 玩家对象，
// 玩家对象 +8 = 他的生物（PlayerIs 0x84d190 同款取法），生物 +8 = 游戏对象，对象 +0x278 最低位 = 有效。
// 以前按名字 goheroa00 遍历 3 万多个对象再挑「防御组多的」：场上有两个同名生物（…9520 真身、…F400），
// 2026-11-15 16:56:20 实测被另一个抢走过 3 秒。运行时核对过：这个指针 = 挨打的那个 …9520。
constexpr uintptr_t kPlayerRva = 0x22e7d50;
uintptr_t NativeHero()
{
    const uintptr_t P = Rd<uintptr_t>(g_exe + kPlayerRva);
    const uintptr_t c = P ? Rd<uintptr_t>(P + 8) : 0;
    const uintptr_t go = c ? Rd<uintptr_t>(c + 8) : 0;
    return go && (Rd<uint32_t>(go + 0x278) & 1) ? c : 0;
}

bool IsHero(uintptr_t creature)
{
    if (!creature)
        return false;
    if (creature == g_hero.load(std::memory_order_relaxed))
        return true;
    if (creature != NativeHero())
        return false;
    g_hero.store(creature, std::memory_order_relaxed);
    g_heroFlips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// 奎托斯当前防御组里有没有弹反体（半径 >0）= 是否在弹反窗口
bool InParryWindow(uintptr_t hero)
{
    const uintptr_t D = Rd<uintptr_t>(hero + 0x3a70);
    const int nSets = D ? Rd<int>(D + 0x48) : 0;
    for (int s = 0; s < nSets && s < 4; ++s) {
        const uintptr_t S = Rd<uintptr_t>(D + 8 + s * 8);
        const int n = S ? Rd<int>(S + 0x80) : 0;
        for (int i = 0; i < n && i < kSetSlots; ++i) {
            const uintptr_t v = Rd<uintptr_t>(S + i * 8);
            if (v && Rd<float>(v + 0x40) > 0.f)
                return true;
        }
    }
    return false;
}

// 攻击者是不是落在奎托斯的「可弹反区域」：读游戏自己的受击区域表（防御组件 +0x10e0，0x724fe0 同款查法），
// 区域哈希 = 弹反体 +0x20 认的 011CBBEB9CE82EB8 就能弹反。实测表：该区域 270°..90°（正前方左右各 90°）。读不到就按 ±90°。
constexpr uint64_t kParryZone = 0x011CBBEB9CE82EB8ull;
bool InParryZone(uintptr_t hero, float deg)
{
    const float a = deg < 0.f ? deg + 360.f : deg;
    const uintptr_t D = Rd<uintptr_t>(hero + 0x3a70);
    if (D) {
        uint64_t key = 0;
        const int nSets = Rd<int>(D + 0x48);
        for (int i = 0; i < nSets && i < 4 && !key; ++i)
            key = Rd<uint64_t>(Rd<uintptr_t>(D + 8 + i * 8) + 0xf8);
        if (!key)
            key = Rd<uint64_t>(Rd<uintptr_t>(D + 0x10d0) + 0x18);
        const uintptr_t M = Rd<uintptr_t>(D + 0x10e0);
        const int n = M ? Rd<int>(M + 0x28) : 0;
        const uintptr_t arr = M ? Rd<uintptr_t>(M + 0x20) : 0;
        for (int i = 0; arr && i < n && i < 64; ++i) {
            if (Rd<uint64_t>(arr + i * 16 + 8) != key)
                continue;
            const uintptr_t list = Rd<uintptr_t>(arr + i * 16);
            for (int k = 1; list && k <= 8; ++k) {
                const uintptr_t z = Rd<uintptr_t>(list + k * 8);
                if (!z)
                    continue;
                const float lo = Rd<float>(z + 8), hi = Rd<float>(z + 0xc);
                const bool in = lo <= hi ? (a >= lo && a <= hi) : (a >= lo || a <= hi);
                if (in)
                    return Rd<uint64_t>(z) == kParryZone;
            }
            break;
        }
    }
    return fabsf(deg) <= 90.f;
}

// ---- 招式时间轴（2026-11-14 快照 + 静态数据对照得出）----
// 生物+0x3a70 = 招式系统：+0x118 当前招式静态数据，+0xd4 本招已进行时间（招式时间，按 +0xe8 播放速率走），+0xe4 整招时长。
// 招式：+0x20 记录指针数组，+0x28 个数；记录 = u32 类型(0x1d 定时动作)、f16 开始、f16 结束（占整招比例）、
// +0x10 动作类型（0x47895f0c39c33568 = 贴标签）、+0x18 参数（标签哈希）。
// 实测巨魔三招：「Attacking」标签的结束 ≈ 出伤（A 1.571 s / 实碰 1.40–1.45；B 1.778 / 1.70；D 0.750 / 0.776）。
constexpr uint64_t kActTag = 0x47895f0c39c33568ull;
constexpr uint64_t kHashAttacking = 0x2a4afc9e7176c0c2ull;
float Half(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    uint32_t bits;
    if (exp == 0)
        bits = sign; // 非规格化数当 0（时间轴用不到这么小）
    else if (exp == 31)
        bits = sign | 0x7f800000u | (man << 13);
    else
        bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
struct MoveTime {
    uintptr_t move = 0;
    float elapsed = -1.f, tagEnd = -1.f, rate = 1.f, dur = 0.f;
};
// 当前招式里、还没结束的那段 Attacking 标签的结束时间（招式时间，秒）。读不到返回 false
bool ReadMoveTime(uintptr_t creature, MoveTime& t)
{
    const uintptr_t ms = Rd<uintptr_t>(creature + 0x3a70);
    if (!ms)
        return false;
    t.move = Rd<uintptr_t>(ms + 0x118);
    t.elapsed = Rd<float>(ms + 0xd4);
    t.dur = Rd<float>(ms + 0xe4);
    t.rate = Rd<float>(ms + 0xe8);
    if (!(t.rate > 0.05f && t.rate < 20.f))
        t.rate = 1.f;
    if (!t.move || !(t.dur > 0.f && t.dur < 120.f))
        return false;
    const uintptr_t arr = Rd<uintptr_t>(t.move + 0x20);
    const uint32_t n = Rd<uint32_t>(t.move + 0x28);
    float best = -1.f;
    for (uint32_t i = 0; arr && i < n && i < 256; ++i) {
        const uintptr_t e = Rd<uintptr_t>(arr + i * 8);
        if (!e || Rd<uint32_t>(e) != 0x1d || Rd<uint64_t>(e + 0x10) != kActTag || Rd<uint64_t>(e + 0x18) != kHashAttacking)
            continue;
        const float end = Half(Rd<uint16_t>(e + 6)) * t.dur;
        if (end >= t.elapsed - 0.3f && (best < 0.f || end < best))
            best = end;
    }
    t.tagEnd = best;
    return best >= 0.f;
}

// ---- 事件环（钩子里只写环，后台线程写日志）----
enum Kind : uint8_t { kPress, kSwallow, kPassWindow, kPassTimeout, kPassRearm, kBlock, kOff, kEvPress, kEvSwallow, kEvPass, kShove, kBackSkip, kHeld, kEvPredHit, kEvPredPass, kProjParry, kProjPre };
struct Rec {
    int64_t us;
    int64_t sincePress;
    uintptr_t attacker, victim;
    Kind kind;
    uint8_t ret, out, cleared;
    float deg, dist;
    uint64_t flags, p5, p6;
    uint16_t aiState;  // 攻击者 AI(生物+0x3a58) +0x1438：被格挡时只有 0x10/0x20/0x400 才会做「被弹开」反应（0x1406b8e50）
    uint8_t react;     // 攻击者 AI 配置(+0x20) 的默认被格挡反应(+0x290) 的类型(+0x28)：0 无反应，1 硬直，2 击退，3/4 其他
    uint8_t matchReact; // 奎托斯这次匹配的防御体(+0x4670) 自带的反应类型(+0x28)，0 = 交给攻击者默认
    float mvElapsed, mvTagEnd, mvRate; // 攻击者当前招式：已进行 / Attacking 标签结束 / 速率（读不到为 -1）
    uintptr_t mvMove;                  // 攻击者当时的招式（v6.22：核对预读的攻击段和真正打中的是不是同一个）
    float mvDur;
    uint32_t caller; // 格挡判定的调用者（GoW+偏移），只有 kBlock 记
};
constexpr uint32_t kRing = 1 << 10;
Rec g_ring[kRing];
std::atomic<uint32_t> g_head{0};

void Push(Kind k, uintptr_t attacker, uintptr_t victim, uint8_t ret = 0, uint8_t out = 0, uint8_t cleared = 0,
    uint64_t flags = 0, uint64_t p5 = 0, uint64_t p6 = 0, uint32_t caller = 0)
{
    Rec& e = g_ring[g_head.fetch_add(1) & (kRing - 1)];
    e.caller = caller;
    e.flags = flags;
    e.p5 = p5;
    e.p6 = p6;
    e.deg = e.dist = -1.f;
    if (attacker && !Bearing(victim, attacker, e.deg, e.dist))
        e.deg = e.dist = -1.f;
    e.us = NowUs();
    const int64_t ps = g_pressStart.load();
    e.sincePress = ps ? e.us - ps : -1;
    e.attacker = attacker;
    e.victim = victim;
    e.kind = k;
    e.ret = ret;
    e.out = out;
    e.cleared = cleared;
    e.aiState = 0xFFFF;
    e.react = e.matchReact = 0xFF;
    e.mvElapsed = e.mvTagEnd = -1.f;
    e.mvRate = 1.f;
    e.mvMove = 0;
    e.mvDur = 0.f;
    if (attacker && k != kBlock && k != kShove) {
        MoveTime mt;
        ReadMoveTime(attacker, mt);
        e.mvElapsed = mt.elapsed;
        e.mvTagEnd = mt.tagEnd;
        e.mvRate = mt.rate;
        e.mvMove = mt.move;
        e.mvDur = mt.dur;
    }
    if (k == kBlock && attacker) {
        const uintptr_t ai = Rd<uintptr_t>(attacker + 0x3a58);
        if (ai) {
            e.aiState = Rd<uint16_t>(ai + 0x1438);
            const uintptr_t cfg = Rd<uintptr_t>(ai + 0x20);
            const uintptr_t d = cfg ? Rd<uintptr_t>(cfg + 0x290) : 0;
            e.react = d ? Rd<uint8_t>(d + 0x28) : 0xFE;
        }
        const uintptr_t vol = Rd<uintptr_t>(victim + 0x4670);
        e.matchReact = vol ? Rd<uint8_t>(vol + 0x28) : 0xFE;
    }
}

// 实测（2026-11-14）：巨魔 gotroll00 的近战，格挡判定在弹反窗口里也通过，但奎托斯不进弹反成功状态（0/9），改走闪避
bool IsHeavyAttacker(uintptr_t creature)
{
    char n[16] = {};
    const uintptr_t go = Rd<uintptr_t>(creature + 8);
    const uintptr_t str = go ? Rd<uintptr_t>(go + 0x38) : 0;
    return str && SafeRead(str, n, 8) && strncmp(n, "gotroll", 7) == 0;
}

// ---- 1. 碰撞→命中入口 0x8c9e30 ----
using ContactFn = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// push rbp / push rsi / push r13 / push r15（共 7 字节）/ lea rbp,[rsp-0x288]
constexpr uint8_t kContactBytes[8] = {0x40, 0x55, 0x56, 0x41, 0x55, 0x41, 0x57, 0x48};
void __fastcall ContactDetour(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Hook g_contact{"碰撞命中", 0x8c9e30, kContactBytes, sizeof(kContactBytes), 7, reinterpret_cast<void*>(&ContactDetour)};

// 0x8c9e30 有个游戏自己的未初始化读：0x8ca2e3 读 [rbp+0x78]（第一次写在 0x8ca3e9），为 0 时才去算 p6 哈希
// （命中记录 +0xd0，格挡/弹反体按它匹配）。平时那块栈恰好是 0；我们的钩子在它下面算过东西会留下残值，
// p6 就变成垃圾，正面也弹反不了。所以调原函数前把下面这块栈清零，恢复成游戏自然调用时的样子。
__declspec(noinline) void ScrubStack()
{
    volatile uint8_t buf[0x1000];
    for (size_t i = 0; i < sizeof(buf); ++i)
        buf[i] = 0;
}

extern std::atomic<int64_t> g_snapLastContact;
extern std::atomic<uintptr_t> g_projSeenAtk; // 最近一次「飞行物那条路」（0x818e10）来的格挡判定：攻击者 / 命中标志 / 时间
extern std::atomic<uint64_t> g_projSeenF;
extern std::atomic<int64_t> g_projSeenUs;
bool GamePaused();
void TakeSnap(uintptr_t creature, uint32_t kind);

constexpr uint64_t kAtkLevelBits = 0x3F8000; // 命中标志里的攻击等级位（第 15..21 位）
constexpr uint64_t kShieldMask = 0x278000;   // 奎托斯盾防御体掩码的并集（第 15/16/17/18/21 位）
// 有攻击等级位、但没有一位落在盾的掩码里 = 盾收不了（红光那一类）。没有等级位的（环境伤害 0x20、阿特柔斯的箭）不归这条管
inline bool NoShield(uint64_t hitFlags) { return (hitFlags & kAtkLevelBits) != 0 && (hitFlags & kShieldMask) == 0; }

void __fastcall ContactDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const uintptr_t victim = Rd<uintptr_t>(b + 0x20);
    bool swallow = false;
    if (IsHero(victim)) {
        const uintptr_t attacker = Rd<uintptr_t>(a + 0x20);
        const int64_t now = NowUs();
        if (now - g_snapLastContact.load() > 100'000) {
            g_snapLastContact.store(now);
            TakeSnap(attacker, 2);
        }
        const int64_t ps = g_pressStart.load();
        float deg = 0.f, dist = 0.f;
        const bool heavy = IsHeavyAttacker(attacker); // 巨魔这类：格挡判定能过但游戏不出弹反（命中标志高位 0004），改闪避
        // 这一刀的攻击定义 c+0x80，+0x44 是 F 标志（0x141126520 枚举表：4 = FUnblockable）。
        // 命中结算 0x8c72a0 里 记录+0x4c（= 攻击定义+0x44）& 4 就把格挡结果清零 → 红光刀只能闪
        const uintptr_t atkDef = Rd<uintptr_t>(c + 0x80);
        // 实测（2026-11-14 Baldur）：红光远程冲拳 +0x44 = 0x4000，没有 4；真正的标志在 +0x48（= 格挡判定的「命中标志」）
        // 高 16 位的 0x0004：红光 0004020000080040 格挡判定 返回 0，巨魔近战也是高位 0004（能挡但不出弹反）。
        // 记录里 atkF 低 32 位放 +0x44，高 32 位放 +0x48 的高 32 位
        const uint32_t f44 = atkDef ? Rd<uint32_t>(atkDef + 0x44) : 0;
        const uint64_t f48 = atkDef ? Rd<uint64_t>(atkDef + 0x48) : 0;
        const uint64_t atkF = (f48 & 0xFFFFFFFF00000000ull) | f44;
        // v6.21：盾挡不挡得住按游戏自己的匹配规则算（0x9140b0：命中标志 & 防御体 +0x10 掩码 ≠ 0 才匹配）。
        // 命中标志第 15..21 位是「攻击等级」；奎托斯 5 个盾防御体（弹反体和普通格挡体一样）掩码合起来 0x278000，
        // 第 19 位（0x80000）哪个都不收 → 举着盾、在弹反窗口里也照样挨打。日志实证：gowitch 的 0000200100080040
        // 9 次全是「返回=0」，Baldur 红光 0004020000080040 同理；带 0x8000/0x10000/0x20000 的全部能弹。
        // v6.23：高 16 位的 0004 不是「不可格挡」。巨魔近战带它照样过格挡判定；gofanatic 的飞行物 0004000000008044
        // 攻击定义和骷髅火球一样（Flags 0x1000、伤害 10、等级位第 15 位在盾掩码里），却被这一位排除在弹反之外，只能碰到才闪。
        // 所以飞行物（命中标志带 kHitProjectile=4，或者刚从飞行物那条路判过格挡）不看这一位；近战没实测过，照旧走闪避。
        const bool isProj = (f48 & 4) != 0 || (attacker && attacker == g_projSeenAtk.load(std::memory_order_relaxed) &&
                                                  f48 == g_projSeenF.load(std::memory_order_relaxed) &&
                                                  now - g_projSeenUs.load(std::memory_order_relaxed) < 100'000);
        const bool unblock = (f44 & 4) != 0 || NoShield(f48) || (!isProj && ((f48 >> 48) & 4) != 0);
        const bool back = (Bearing(victim, attacker, deg, dist) && !InParryZone(victim, deg)) || heavy || unblock;
        const int64_t es = g_evadeStart.load();
        if (back && g_evadeOn.load(std::memory_order_relaxed)) {
            const bool sameSwing = attacker == g_evAttacker.load() && now - g_evLast.load() < kEvadeGapUs && es &&
                                   now - es < kEvadeMaxUs;
            const bool predicted = attacker == g_evAttacker.load() && now < g_predUntil.load();
            if (predicted && !g_predSwallow.load(std::memory_order_relaxed)) {
                // 实验：预判闪避中的碰撞不吞，交给游戏自己判（游戏有 HitModifierEvade / EvadeSucceedReaction，闪避中可能本来就不吃伤害）
                if (now - g_evLast.load() > kEvadeGapUs)
                    Push(kEvPredPass, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
                g_evLast.store(now);
            } else if (predicted && !sameSwing) { // 预判闪了还是碰到：吞掉兜底，并记下来看提前量够不够
                g_evLast.store(now);
                g_predUntil.store(0);
                swallow = true;
                Push(kEvPredHit, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
            } else if (sameSwing) {
                g_evLast.store(now);
                swallow = true;
                Push(kEvSwallow, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
            } else if ((es == 0 || now - es > kEvadeRearmUs) && !GamePaused()) {
                g_evAttacker.store(attacker);
                g_evLast.store(now);
                float hp[3], ap[3];
                if (WorldPos(Rd<uintptr_t>(victim + 8), hp) && WorldPos(Rd<uintptr_t>(attacker + 8), ap)) {
                    const float dx = hp[0] - ap[0], dz = hp[2] - ap[2], l = sqrtf(dx * dx + dz * dz);
                    g_evadeDirX.store(l > 0.f ? dx / l : 0.f);
                    g_evadeDirZ.store(l > 0.f ? dz / l : 0.f);
                }
                g_evadeRollNow.store(false);
                g_evadeStart.store(now);
                swallow = true;
                Push(kEvPress, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
            } else if (!GamePaused()) {
                // 上一次闪避还在冷却（含预判闪避刚按下、还没登记成「预判」的那一帧）：不再按键，但这一刀照样吞
                g_evLast.store(now);
                swallow = true;
                Push(kEvSwallow, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
            } else {
                Push(kEvPass, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
            }
        } else if (back) {
            Push(kBackSkip, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        } else if (!g_parryOn.load(std::memory_order_relaxed)) {
            Push(kOff, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        } else if (g_l1Natural.load(std::memory_order_relaxed) && !(ps && now - ps < kHoldUs)) {
            Push(kHeld, attacker, victim, 0, 0, 0, atkF, f48, atkDef); // 实测：按着盾时替他松开再按，碰撞只持续几十毫秒，来不及进弹反窗口，反而吞掉了格挡
        } else if (InParryWindow(victim)) {
            if (g_passAttacker.exchange(attacker) != attacker || now - g_passUs.load() > 300'000)
                g_parryTry.fetch_add(1);
            g_passUs.store(now);
            Push(kPassWindow, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        } else if (ps == 0 || now - ps > kRearmUs) {
            g_releaseFirst.store(false);
            g_pressStart.store(now);
            swallow = true;
            Push(kPress, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        } else if (now - ps < kSwallowUs) {
            swallow = true;
            Push(kSwallow, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        } else {
            Push(now - ps < kRearmUs ? kPassTimeout : kPassRearm, attacker, victim, 0, 0, 0, atkF, f48, atkDef);
        }
    }
    if (!swallow) {
        ScrubStack();
        reinterpret_cast<ContactFn>(g_contact.tramp)(a, b, c, d, e);
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
}

// ---- 防御组变化记录（在游戏线程每次条件判定时采样，变了才记）----
struct DefRec {
    int64_t us;
    uint8_t what; // 0 防御组变化，1 × 按下，2 × 松开
    int n;
    uintptr_t vols[8];
};
constexpr uint32_t kDefRing = 1 << 10;
DefRec g_defRing[kDefRing];
std::atomic<uint32_t> g_defHead{0};
std::atomic<uint64_t> g_defSig{0};
std::atomic<uint8_t> g_evadeBtn{0};
uintptr_t g_curVols[8];
std::atomic<int> g_curN{0};

void PushDef(uint8_t what)
{
    DefRec& e = g_defRing[g_defHead.fetch_add(1) & (kDefRing - 1)];
    e.us = NowUs();
    e.what = what;
    e.n = g_curN.load();
    memcpy(e.vols, g_curVols, sizeof(e.vols));
}

void SampleDefense()
{
    const uintptr_t hero = g_hero.load(std::memory_order_relaxed);
    if (!hero)
        return;
    const uintptr_t D = Rd<uintptr_t>(hero + 0x3a70);
    const uintptr_t S = D && Rd<int>(D + 0x48) > 0 ? Rd<uintptr_t>(D + 8) : 0;
    int n = S ? Rd<int>(S + 0x80) : 0;
    n = n < 0 ? 0 : (n > 8 ? 8 : n);
    uintptr_t v[8] = {};
    if (n)
        SafeRead(S, v, n * sizeof(uintptr_t));
    uint64_t sig = 1469598103934665603ull ^ static_cast<uint64_t>(n);
    for (int i = 0; i < n; ++i)
        sig = (sig ^ v[i]) * 1099511628211ull;
    if (g_defSig.exchange(sig) == sig)
        return;
    memcpy(g_curVols, v, sizeof(v));
    g_curN.store(n);
    PushDef(0);
}

// ---- 虚拟按键：往奎托斯的手柄状态里写按键位（和真按一样，游戏各处都看得到）----
// 条件判定上下文 ctx：+0 生物，+0x50 → 输入上下文，其 +0x10 → 手柄对象 pad。
// pad +0x375 非 0 用 +0x10 那份按键状态，否则 +0xe8（同 0x1400c11e0）；状态 +0 = 当前帧按键位，+0x48 起是历史环。
// 「按下」= 当前帧有、上一帧历史没有（0x1400f42b0）。每帧游戏读设备会覆盖当前帧，所以按住期间每次判定前都 OR 一次。
// 按键位（动作 ID 表 0xd85c30）：× 0x40，□ 0x80，△ 0x10，○ 0x20，L1 0x04，R1 0x08。
constexpr uint32_t kBtnCross = 0x40;
std::atomic<uint32_t> g_padWrites{0};
std::atomic<uint32_t> g_padCalls{0}, g_padNotHero{0}, g_padNoPad{0}, g_padAlready{0};
std::atomic<uintptr_t> g_padLastCreature{0}, g_padLastS{0}, g_padLastPad{0};
// 左摇杆（输入上下文 +0x410/414/418 = 世界坐标方向 x/y/z，+0x41c = 力度；0x14087b770 拿它和奎托斯朝向做点积/叉积判前后左右）
std::atomic<uintptr_t> g_heroCtl{0};
std::atomic<bool> g_evadeStick{true};
std::atomic<uint32_t> g_stickWrites{0};

std::atomic<int64_t> g_heroCondUs{0}; // 最近一次游戏给奎托斯做按键判定的时刻：菜单/暂停里不做，用来认「游戏在跑」

void PressPadButtons(uintptr_t ctx) // 只记下奎托斯的输入上下文（摇杆被动记录用）；真正的按键在输入轮询钩子里写
{
    if (!ctx)
        return;
    const uintptr_t c = Rd<uintptr_t>(ctx);
    if (c && c == g_hero.load(std::memory_order_relaxed))
        g_heroCondUs.store(NowUs(), std::memory_order_relaxed);
    if (g_heroCtl.load(std::memory_order_relaxed))
        return;
    if (c && c == g_hero.load(std::memory_order_relaxed))
        g_heroCtl.store(Rd<uintptr_t>(ctx + 0x50), std::memory_order_relaxed);
}

// ---- 每帧输入轮询 0x14086b2c0 ----
// 奎托斯的手柄 = 全局 0x142506320（游戏读键盘/手柄后在这里更新）。0x140438770(手柄, 原始键号, 按压量 0..255) 是游戏自己的
// 单键置位：写 +0x68[键]、+0x10 原始掩码、按重映射表 +0x318 写 +0x140[键] 和 +0xe8 掩码。× = 键号 6（掩码 0x40）。
// 左摇杆：+0x1c8 原始 (x,y)（相对镜头，W = (0,1)），+0x1d0/+0x1d8/+0x1f0 平滑副本，+0x1e0 角度(度, atan2(x,y))，
// +0x1e4 力度，+0x1e8 单位方向。轮询跑完立即覆盖，后面的平滑/世界方向换算/动作判定看到的就是我们给的值。
constexpr uintptr_t kPadRva = 0x2506320;
constexpr uintptr_t kSetButtonRva = 0x438770;
constexpr uintptr_t kCamMgrRva = 0x22a93d0; // 相机：+0xaa0 世界矩阵，行0 右、行2 后
constexpr uint32_t kCrossKey = 6;
using SetButtonFn = void(__fastcall*)(uintptr_t pad, uint32_t key, uint8_t value);
std::atomic<bool> g_crossDown{false};
std::atomic<uint32_t> g_pollCalls{0};

bool WorldToStick(float wx, float wz, float& sx, float& sy)
{
    const uintptr_t cam = Rd<uintptr_t>(g_exe + kCamMgrRva);
    float M[12] = {};
    if (!cam || !SafeRead(cam + 0xaa0, M, sizeof M))
        return false;
    float rx = M[0], rz = M[2], fx = -M[8], fz = -M[10];
    const float rl = sqrtf(rx * rx + rz * rz), fl = sqrtf(fx * fx + fz * fz);
    if (rl < 1e-4f || fl < 1e-4f)
        return false;
    sx = (wx * rx + wz * rz) / rl;
    sy = (wx * fx + wz * fz) / fl;
    const float l = sqrtf(sx * sx + sy * sy);
    if (l < 1e-4f)
        return false;
    sx /= l;
    sy /= l;
    return true;
}

std::atomic<uint32_t> g_condWrites{0};
void WriteStick(uintptr_t pad)
{
    float sx, sy;
    if (!g_evadeStick.load(std::memory_order_relaxed) ||
        !WorldToStick(g_evadeDirX.load(std::memory_order_relaxed), g_evadeDirZ.load(std::memory_order_relaxed), sx, sy))
        return;
    float* f = reinterpret_cast<float*>(pad + 0x1c8);
    f[0] = sx, f[1] = sy;               // 1c8 原始
    f[2] = sx, f[3] = sy;               // 1d0
    f[4] = sx, f[5] = sy;               // 1d8
    f[6] = atan2f(sx, sy) * 57.29578f;  // 1e0 角度
    f[7] = 1.f;                         // 1e4 力度
    f[8] = sx, f[9] = sy;               // 1e8 单位方向
    f[10] = sx, f[11] = sy;             // 1f0
    g_stickWrites.fetch_add(1, std::memory_order_relaxed);
}

// 按键判定那一刻（游戏真正读按键的时候）再写一遍：v5.6 实测轮询后写的会在读之前被重算覆盖
void ApplyVirtualPadAtCond(uintptr_t ctx)
{
    const int phase = EvadePhase();
    if (!phase || !ctx)
        return;
    if (Rd<uintptr_t>(ctx) != g_hero.load(std::memory_order_relaxed))
        return;
    const uintptr_t pad = g_exe + kPadRva;
    if (phase == 1) {
        g_condSawPress.store((g_evadeStart.load(std::memory_order_relaxed) << 2) | EvadePressIdx(), std::memory_order_relaxed);
        *reinterpret_cast<volatile uint32_t*>(pad + 0xe8) |= 0x40;
        *reinterpret_cast<volatile uint32_t*>(pad + 0x10) |= 0x40;
        reinterpret_cast<volatile uint32_t*>(pad + 0x68)[kCrossKey] = 255;
        reinterpret_cast<volatile uint32_t*>(pad + 0x140)[kCrossKey] = 255;
    }
    WriteStick(pad);
    if (const uintptr_t ctl = Rd<uintptr_t>(ctx + 0x50)) { // 世界方向的摇杆（0x14087b770 读的那份）
        float* v = reinterpret_cast<float*>(ctl + 0x410);
        v[0] = g_evadeDirX.load(std::memory_order_relaxed);
        v[1] = 0.f;
        v[2] = g_evadeDirZ.load(std::memory_order_relaxed);
        v[3] = 1.f;
    }
    g_condWrites.fetch_add(1, std::memory_order_relaxed);
}

void ApplyVirtualPad()
{
    const uintptr_t pad = g_exe + kPadRva;
    const auto setButton = reinterpret_cast<SetButtonFn>(g_exe + kSetButtonRva);
    const int phase = EvadePhase();
    if (phase != 1 && g_crossDown.exchange(false, std::memory_order_relaxed))
        setButton(pad, kCrossKey, 0); // 松开
    if (!phase)
        return;
    g_pollCalls.fetch_add(1, std::memory_order_relaxed);
    g_padLastPad.store(pad, std::memory_order_relaxed);
    g_padLastS.store(pad + 0xe8, std::memory_order_relaxed);
    // 实测 v6.6：第二下如果先被输入轮询写进去，按键历史里就已经有 ×，游戏认不出「刚按下」，翻滚接不上（6 次只成 1 次）。
    // 所以每一下都等按键判定钩子先写过（游戏先看到「刚按下」），输入轮询才跟着按住
    // 只管第二下：第一下要是等按键判定，边跑边闪时（那时按键判定不一定跑）就按不出来了（v5.4/5.6 的教训）
    const int idx = EvadePressIdx();
    if (phase == 1 && (idx == 1 || g_condSawPress.load(std::memory_order_relaxed) ==
                                       ((g_evadeStart.load(std::memory_order_relaxed) << 2) | idx))) {
        setButton(pad, kCrossKey, 255);
        g_crossDown.store(true, std::memory_order_relaxed);
    }
    g_padWrites.fetch_add(1, std::memory_order_relaxed);
    WriteStick(pad);
}

// 被动记录：每帧轮询后手柄状态有变化就记一条（真按闪避时的样子，用来对照虚拟按键）
struct PadRec {
    int64_t us;
    uint32_t e8, raw10, nz140; // nz140：+0x140 起 32 个按压量里非 0 的位
    float sx, sy, mag;
    uint32_t v140[8];
};
constexpr uint32_t kPadRing = 256;
PadRec g_padRing[kPadRing];
std::atomic<uint32_t> g_padHead{0};
uint32_t g_lastE8 = 0xFFFFFFFF;

void SamplePad()
{
    const uintptr_t pad = g_exe + kPadRva;
    const uint32_t e8 = *reinterpret_cast<uint32_t*>(pad + 0xe8);
    if (e8 == g_lastE8)
        return;
    if ((e8 & 0x40) && !(g_lastE8 & 0x40) && !EvadePhase())
        g_manualCrossUs.store(NowUs(), std::memory_order_relaxed); // 你自己按下 ×
    g_lastE8 = e8;
    PadRec& r = g_padRing[g_padHead.fetch_add(1) & (kPadRing - 1)];
    r.us = NowUs();
    r.e8 = e8;
    r.raw10 = *reinterpret_cast<uint32_t*>(pad + 0x10);
    const uint32_t* v = reinterpret_cast<uint32_t*>(pad + 0x140);
    r.nz140 = 0;
    for (int i = 0; i < 32; ++i)
        if (v[i])
            r.nz140 |= 1u << i;
    for (int i = 0; i < 8; ++i)
        r.v140[i] = v[i];
    const float* f = reinterpret_cast<float*>(pad + 0x1c8);
    r.sx = f[0];
    r.sy = f[1];
    r.mag = f[7];
}

// ---- 出招预警（被动记录）：照 HUD 危险提示圈 0x1406eefa0 的判断 ----
// 敌人列表：表头 = GoW+0x118d980 + (u32)GoW+0x506c76c * 16；节点 n：AI 对象 = n[3]（n 为 *link - 8），生物 = *(AI+8)，下一个 = *(n+8) - 8。
// 目标 = 0x1400f8310(AI)；标签查询 0x1405041e0(游戏对象, 哈希)。哈希：h=0；每字符(小写转大写) h=(h+c)*0x401；h^=h>>6。
constexpr uint64_t TagHash(const char* s)
{
    uint64_t h = 0;
    for (; *s; ++s) {
        uint64_t c = static_cast<uint8_t>(*s);
        if (c >= 'a' && c <= 'z')
            c -= 0x20;
        h = (c + h) * 0x401;
        h ^= h >> 6;
    }
    return h;
}
constexpr uint64_t kTagAttacking = TagHash("Attacking");
constexpr uint64_t kTagMelee = TagHash("Melee");
static_assert(TagHash("Attacking") == kHashAttacking);
using HasTagFn = bool(__fastcall*)(uintptr_t go, uint64_t hash);
using TargetFn = uintptr_t(__fastcall*)(uintptr_t ai);
// 招式里预读出来的一段攻击（说明见下面「招式预读」）
struct AtkSeg {
    float on, off;   // 占整招的比例；冲击波是脚本记录的起止
    uint32_t flags;  // Collision +0x44
    uint64_t hf;     // Collision +0x48
    uintptr_t def;   // Collision 地址，命中时和碰撞上下文 +0x80 对得上
    uint8_t kind;    // 0 近战攻击段，1 冲击波
};
struct WarnRec {
    int64_t us;
    uintptr_t creature;
    bool start, melee;
    float deg, dist;
    uint8_t what; // 0 标签开始/结束，1 预判闪避
    float remain, elapsed, tagEnd, rate;
    uintptr_t move; // 当时的招式（v6.22：后台线程拿它预读攻击段写日志）
    float dur;
    bool hasSeg;    // 预判闪避是按哪一段攻击判的
    AtkSeg seg;
};
constexpr uint32_t kWarnRing = 256;
WarnRec g_warnRing[kWarnRing];
std::atomic<uint32_t> g_warnHead{0};
constexpr int kMaxFoes = 32;
uintptr_t g_attackingNow[kMaxFoes];
int g_attackingN = 0;

// ---- 招式系统快照（只记录）：找「当前招式 / 已进行时间」字段用 ----
// 生物+0x3a70 = ms::MoveSystemMutable，+0x3b90 = tAnimSystem。出招期间每帧拍一份，刀碰到时再拍一份，后台线程落盘。
constexpr size_t kSnapMs = 0x1400, kSnapAnim = 0x800;
struct Snap {
    std::atomic<uint32_t> seq;
    uint32_t kind; // 0 出招中逐帧 1 出招开始 2 碰到奎托斯
    int64_t us;
    uintptr_t creature, ms, anim;
    uint8_t msBytes[kSnapMs];
    uint8_t animBytes[kSnapAnim];
};
constexpr uint32_t kSnapRing = 512;
Snap* g_snaps = nullptr; // 后台线程启动时分配
std::atomic<uint32_t> g_snapHead{0};
std::atomic<int64_t> g_snapLastContact{0};

void TakeSnap(uintptr_t creature, uint32_t kind)
{
    if (!g_snaps || !g_snapOn.load(std::memory_order_relaxed))
        return;
    const uint32_t i = g_snapHead.fetch_add(1);
    Snap& s = g_snaps[i % kSnapRing];
    s.kind = kind;
    s.us = NowUs();
    s.creature = creature;
    s.ms = Rd<uintptr_t>(creature + 0x3a70);
    s.anim = Rd<uintptr_t>(creature + 0x3b90);
    if (!s.ms || !SafeRead(s.ms, s.msBytes, kSnapMs))
        memset(s.msBytes, 0, kSnapMs);
    if (!s.anim || !SafeRead(s.anim, s.animBytes, kSnapAnim))
        memset(s.animBytes, 0, kSnapAnim);
    s.seq.store(i + 1, std::memory_order_release);
}

// ---- 招式预读（v6.22）：敌人一进招式，这一招会打出来的每一段攻击就已经在招式的静态数据里了，不用等命中 ----
// 招式 Move（exe 字段描述表里的结构 0x3ca）：+0x00 BranchList、+0x10 CollisionList{指针数组, u32 个数}、+0x20 ActionList{同}、+0x40 MoveName。
// 攻击段 Collision（结构 0x12f，就是碰撞上下文 +0x80 那个「攻击定义」）：+0 On / +2 Off（f16，占整招比例，实测 × 运行时时长
//   能对上日志里的接触时刻）、+0x08 Damage、+0x44 Flags（4 = kFUnblockable，0x800 = kFOnlyHitAI）、+0x48 HitFlags。
// 冲击波：时间轴里的 kTScript(0x1d) 记录，动作哈希 = "SCR_Concussion"，记录 +0x18 → 冲击波数据（结构 0xc5），它的 +0x38 → Collision
//   （SCR_Concussion 的更新函数 0x296690 就是这么取的）。动作哈希和标签哈希是同一个算法（注册处 0x296ae0）。
// 容器记录（kTContainer = 1）自己带一份子记录数组（+0x20 / +0x28，和招式的 ActionList 同偏移），要递归进去。
constexpr uint64_t kActConcussion = TagHash("SCR_Concussion");
static_assert(kActConcussion == 0xfd0a8735bc354673ull);
static_assert(TagHash("SCR_AddMarker") == kActTag);
constexpr int kMaxSegs = 12;
struct MoveAtk {
    uintptr_t move = 0;
    int n = 0;
    AtkSeg s[kMaxSegs];
};
inline bool SegUnblockable(const AtkSeg& s) { return (s.flags & 4) != 0 || ((s.hf >> 48) & 4) != 0 || NoShield(s.hf); }
// 0x800 = kFOnlyHitAI，0x8000 = kFIgnoreHostile：敌人招式里带这两位的段打不到玩家（内存里 1935 条冲击波记录有 674 条是 OnlyHitAI）
inline bool SegHitsPlayer(const AtkSeg& s) { return (s.flags & 0x8800) == 0; }

void AddSeg(MoveAtk& out, uintptr_t coll, float on, float off, uint8_t kind)
{
    if (!coll || out.n >= kMaxSegs)
        return;
    out.s[out.n++] = {on, off, Rd<uint32_t>(coll + 0x44), Rd<uint64_t>(coll + 0x48), coll, kind};
}

void WalkActions(MoveAtk& out, uintptr_t arr, uint32_t n, int depth, int& budget)
{
    for (uint32_t i = 0; arr && i < n && i < 256 && budget > 0 && out.n < kMaxSegs; ++i, --budget) {
        const uintptr_t e = Rd<uintptr_t>(arr + i * 8);
        if (!e)
            continue;
        const uint32_t type = Rd<uint32_t>(e) & 0xff;
        if (type == 1 && depth < 3) {
            WalkActions(out, Rd<uintptr_t>(e + 0x20), Rd<uint32_t>(e + 0x28), depth + 1, budget);
        } else if (type == 0x1d && Rd<uint64_t>(e + 0x10) == kActConcussion) {
            const uintptr_t conc = Rd<uintptr_t>(e + 0x18);
            AddSeg(out, conc ? Rd<uintptr_t>(conc + 0x38) : 0, Half(Rd<uint16_t>(e + 4)), Half(Rd<uint16_t>(e + 6)), 1);
        }
    }
}

void ReadMoveAtk(uintptr_t move, MoveAtk& out)
{
    out.move = move;
    out.n = 0;
    if (!move)
        return;
    const uintptr_t colls = Rd<uintptr_t>(move + 0x10);
    const uint32_t nc = Rd<uint32_t>(move + 0x18);
    for (uint32_t i = 0; colls && i < nc && i < 32; ++i) {
        const uintptr_t c = Rd<uintptr_t>(colls + i * 8);
        if (c)
            AddSeg(out, c, Half(Rd<uint16_t>(c)), Half(Rd<uint16_t>(c + 2)), 0);
    }
    int budget = 600;
    WalkActions(out, Rd<uintptr_t>(move + 0x20), Rd<uint32_t>(move + 0x28), 0, budget);
}

// 接下来会打到奎托斯的那一段：能打玩家、还没结束的里面开始得最早的。没有返回空
const AtkSeg* NextThreat(const MoveAtk& a, float elapsed, float dur)
{
    const AtkSeg* best = nullptr;
    for (int i = 0; i < a.n; ++i) {
        const AtkSeg& s = a.s[i];
        if (!SegHitsPlayer(s))
            continue;
        // 冲击波在脚本记录开始那一刻就放出去了，过了就不算；近战段看结束
        const float last = (s.kind == 1 ? s.on : s.off) * dur;
        if (last < elapsed - 0.1f)
            continue;
        if (!best || s.on < best->on)
            best = &s;
    }
    return best;
}

// 日志用：一段攻击写成一行字
void FmtSeg(char* buf, size_t n, const AtkSeg& s, float dur)
{
    const char* verdict = !SegHitsPlayer(s) ? "只打 AI，不算" :
        (s.flags & 4) ? "不可格挡（红光）→ 闪"
        : NoShield(s.hf) ? "盾的掩码接不了 → 闪"
        : ((s.hf >> 48) & 4) ? "重击档（高位 0004）→ 照旧闪" : "盾接得住 → 弹反";
    if (s.kind == 1)
        sprintf_s(buf, n, "冲击波 第 %.3f s 放出（记录 %.3f–%.3f） Flags %X 命中标志 %016llX，%s", s.on * dur, s.on, s.off, s.flags,
            static_cast<unsigned long long>(s.hf), verdict);
    else
        sprintf_s(buf, n, "近战段 出伤 %.3f–%.3f s（%.3f–%.3f） Flags %X 命中标志 %016llX，%s", s.on * dur, s.off * dur, s.on, s.off, s.flags,
            static_cast<unsigned long long>(s.hf), verdict);
}

// 每个正在出招的敌人记一份「这一下已经预判闪过」，同一招同一段标签只闪一次；标签灭了或招式重来就清掉
struct PredKey {
    uintptr_t creature, move;
    float tagEnd, lastElapsed;
    bool fired;
    MoveAtk atk; // 这一招预读出来的攻击段
};
PredKey g_predKeys[kMaxFoes];
int g_predN = 0;

PredKey& PredFor(uintptr_t c)
{
    for (int i = 0; i < g_predN; ++i)
        if (g_predKeys[i].creature == c)
            return g_predKeys[i];
    PredKey& k = g_predKeys[g_predN < kMaxFoes ? g_predN++ : kMaxFoes - 1];
    k.creature = c;
    k.move = 0;
    k.tagEnd = k.lastElapsed = -1.f;
    k.fired = false;
    k.atk.move = 0;
    k.atk.n = 0;
    return k;
}

void PushWarn(uint8_t what, uintptr_t hero, uintptr_t c, bool start, bool melee, const MoveTime* mt = nullptr, float remain = 0.f,
    const AtkSeg* seg = nullptr)
{
    WarnRec& r = g_warnRing[g_warnHead.fetch_add(1) & (kWarnRing - 1)];
    r.us = NowUs();
    r.creature = c;
    r.start = start;
    r.melee = melee;
    r.what = what;
    r.remain = remain;
    r.elapsed = mt ? mt->elapsed : -1.f;
    r.tagEnd = mt ? mt->tagEnd : -1.f;
    r.rate = mt ? mt->rate : 1.f;
    r.move = mt ? mt->move : 0;
    r.dur = mt ? mt->dur : 0.f;
    r.hasSeg = seg != nullptr;
    if (seg)
        r.seg = *seg;
    if (!Bearing(hero, c, r.deg, r.dist))
        r.deg = r.dist = -1.f;
}

// 暂停/菜单里：奎托斯手柄的按键历史头 = 0xffff（这时所有按键条件都不成立）。实测 v6.0 暂停时碰撞还在报，
// 不判断的话会每 2 s 替你按一次 ×，菜单里 × 是确认键
// 注：v6.4d 试过再加「0.3 s 没有奎托斯按键判定就算暂停」，结果非战斗走动时也成立（那时游戏不做这项判定），已撤回
bool GamePaused()
{
    return Rd<uint16_t>(g_exe + kPadRva + 0xe8 + 0x52) == 0xffff;
}
std::atomic<bool> g_evadeSide{true}; // ini evade_side：巨魔这类往侧面闪（实测往后闪 1.7 m 躲不开它 4~5 m 的下砸）
std::atomic<bool> g_preread{true};   // ini preread：按招式数据预读攻击段，接不住的（红光等）提前闪（v6.22）

// 预判：近战、该走闪避（背后半圈或巨魔这类弹不了的）、在范围内、离出伤剩不到 predict_ms → 现在就按 × 往背离方向闪
void MaybePredictEvade(uintptr_t hero, uintptr_t c, bool melee)
{
    if (!g_evadeOn.load(std::memory_order_relaxed) || !g_predictOn.load(std::memory_order_relaxed) || GamePaused())
        return;
    MoveTime mt;
    if (!ReadMoveTime(c, mt))
        return;
    PredKey& k = PredFor(c);
    if (k.move != mt.move || fabsf(k.tagEnd - mt.tagEnd) > 1e-3f || mt.elapsed < k.lastElapsed - 0.05f) {
        k.move = mt.move;
        k.tagEnd = mt.tagEnd;
        k.fired = false;
    }
    if (k.atk.move != mt.move)
        ReadMoveAtk(mt.move, k.atk); // 每招只读一次
    k.lastElapsed = mt.elapsed;
    if (k.fired)
        return;
    // v6.22：从招式数据里预读「接下来打过来的那一段」盾接不接得住，不用等命中才知道
    const AtkSeg* next = g_preread.load(std::memory_order_relaxed) ? NextThreat(k.atk, mt.elapsed, mt.dur) : nullptr;
    const bool unblock = next && SegUnblockable(*next);
    if (!melee && !unblock)
        return; // 非近战的招，只管预读出来接不住的；其余还是留给飞行物那条路
    // 出伤时刻：近战按 Attacking 标签结束（实测贴近真实接触）；冲击波按脚本记录开始（那一刻放出去）
    const float hitAt = (unblock && next->kind == 1) ? next->on * mt.dur : mt.tagEnd;
    const float remain = (hitAt - mt.elapsed) / mt.rate; // 真实秒
    // 翻滚要等第二下 ×（第一下后约 0.3 s）才接上，所以巨魔类用更大的提前量 predict_roll_ms
    const bool rollThis = IsHeavyAttacker(c) && g_evadeRoll.load(std::memory_order_relaxed);
    const int leadMs = rollThis ? g_predictRollMs.load(std::memory_order_relaxed) : g_predictLeadMs.load(std::memory_order_relaxed);
    if (remain > leadMs / 1000.f || remain < -0.05f)
        return;
    float deg = 0.f, dist = 0.f;
    if (!Bearing(hero, c, deg, dist) || dist * 10.f > static_cast<float>(g_predictDistDm.load(std::memory_order_relaxed)))
        return;
    const bool heavy = IsHeavyAttacker(c);
    if (!heavy && !unblock && InParryZone(hero, deg))
        return; // 正面可弹反的，留给碰撞时弹反
    const int64_t now = NowUs(), es = g_evadeStart.load();
    if (es && now - es < kEvadeRearmUs)
        return; // 上一次闪避还没做完
    k.fired = true;
    float hp[3], ap[3], AL[12];
    const uintptr_t ag = Rd<uintptr_t>(c + 8);
    if (WorldPos(Rd<uintptr_t>(hero + 8), hp) && WorldPos(ag, ap)) {
        float dx = hp[0] - ap[0], dz = hp[2] - ap[2];
        if (heavy && g_evadeSide.load(std::memory_order_relaxed) && SafeRead(ag + 0x60, AL, sizeof(AL))) {
            // 侧闪：去掉「沿敌人正前方」的分量，只留横向，往奎托斯已经偏的那一侧继续闪；正对着就往敌人右手边
            float fx = -AL[8], fz = -AL[10];
            const float fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-3f) {
                fx /= fl;
                fz /= fl;
                const float along = dx * fx + dz * fz;
                dx -= along * fx;
                dz -= along * fz;
                if (dx * dx + dz * dz < 0.25f * 0.25f) {
                    dx = AL[0];
                    dz = AL[2];
                }
            }
        }
        const float l = sqrtf(dx * dx + dz * dz);
        g_evadeDirX.store(l > 0.f ? dx / l : 0.f);
        g_evadeDirZ.store(l > 0.f ? dz / l : 0.f);
    }
    g_evAttacker.store(c);
    g_evLast.store(now);
    g_predUntil.store(now + static_cast<int64_t>((remain > 0.f ? remain : 0.f) * 1e6f) + 600'000);
    g_evadeRollNow.store(heavy && g_evadeRoll.load(std::memory_order_relaxed));
    g_evadeStart.store(now);
    PushWarn(1, hero, c, true, melee, &mt, remain, next);
}

// 奎托斯自己的招式切换（闪避开始后 1.5 s 内）：找闪避/翻滚是哪招、第二下 × 什么时候按才翻滚
struct HeroMoveRec {
    int64_t sinceEvade;
    uintptr_t move;
    uint64_t hash;
    float elapsed, dur;
    uint32_t phase;
    bool manual;
};
constexpr uint32_t kHmRing = 256;
HeroMoveRec g_hmRing[kHmRing];
std::atomic<uint32_t> g_hmHead{0};
uintptr_t g_hmLastMove = 0;
void TraceHeroMove(uintptr_t hero)
{
    const int64_t man = g_manualCrossUs.load(std::memory_order_relaxed);
    const int64_t es = (std::max)(g_evadeStart.load(std::memory_order_relaxed), man);
    if (!es || NowUs() - es > 1'500'000)
        return;
    const uintptr_t ms = Rd<uintptr_t>(hero + 0x3a70);
    const uintptr_t mv = ms ? Rd<uintptr_t>(ms + 0x118) : 0;
    if (mv == g_hmLastMove)
        return;
    g_hmLastMove = mv;
    HeroMoveRec& r = g_hmRing[g_hmHead.fetch_add(1) & (kHmRing - 1)];
    r.sinceEvade = NowUs() - es;
    r.manual = es == man;
    r.move = mv;
    r.hash = ms ? Rd<uint64_t>(ms + 0x128) : 0;
    r.elapsed = ms ? Rd<float>(ms + 0xd4) : -1.f;
    r.dur = ms ? Rd<float>(ms + 0xe4) : -1.f;
    r.phase = static_cast<uint32_t>(EvadePhase());
}

void ScanWarnings()
{
    const uintptr_t hero = g_hero.load(std::memory_order_relaxed);
    if (!hero)
        return;
    TraceHeroMove(hero);
    const auto hasTag = reinterpret_cast<HasTagFn>(g_exe + 0x5041e0);
    const auto target = reinterpret_cast<TargetFn>(g_exe + 0xf8310);
    const uintptr_t head = g_exe + 0x118d980 + static_cast<uintptr_t>(Rd<uint32_t>(g_exe + 0x506c76c)) * 16;
    uintptr_t now[kMaxFoes];
    int n = 0;
    uintptr_t link = Rd<uintptr_t>(head);
    for (int guard = 0; link && link != head && guard < 512; ++guard) {
        const uintptr_t node = link - 8;
        const uintptr_t ai = Rd<uintptr_t>(node + 0x18);
        link = Rd<uintptr_t>(node + 8);
        const uintptr_t c = ai ? Rd<uintptr_t>(ai + 8) : 0;
        if (!c || c == hero || target(ai) != hero)
            continue;
        const uintptr_t go = Rd<uintptr_t>(c + 8);
        if (!go || !hasTag(go, kTagAttacking) || n >= kMaxFoes)
            continue;
        now[n++] = c;
        bool was = false;
        for (int i = 0; i < g_attackingN; ++i)
            was |= g_attackingNow[i] == c;
        TakeSnap(c, was ? 0 : 1);
        const bool melee = hasTag(go, kTagMelee);
        if (!was) {
            MoveTime mt;
            ReadMoveTime(c, mt);
            PushWarn(0, hero, c, true, melee, &mt);
        }
        MaybePredictEvade(hero, c, melee);
    }
    for (int i = 0; i < g_attackingN; ++i) {
        bool still = false;
        for (int j = 0; j < n; ++j)
            still |= now[j] == g_attackingNow[i];
        if (!still) {
            PushWarn(0, hero, g_attackingNow[i], false, false);
            for (int k = 0; k < g_predN; ++k)
                if (g_predKeys[k].creature == g_attackingNow[i])
                    g_predKeys[k] = g_predKeys[--g_predN];
        }
    }
    memcpy(g_attackingNow, now, sizeof(uintptr_t) * n);
    g_attackingN = n;
}

using PollFn = void(__fastcall*)(uint64_t);
extern Hook g_poll;
void __fastcall PollDetour(uint64_t a)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    reinterpret_cast<PollFn>(g_poll.tramp)(a);
    __try {
        SamplePad();
        ApplyVirtualPad();
        ScanWarnings();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
}
const uint8_t kPollBytes[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x48, 0x83, 0xec, 0x38};
Hook g_poll{"输入轮询", 0x86b2c0, kPollBytes, sizeof(kPollBytes), 5, reinterpret_cast<void*>(&PollDetour)};

// ---- QTE 记录器（只记录不改行为）：奎托斯的按键条件判定原始结果，后台按提示对象去重写 gow4_qte.log ----
struct QRec {
    int64_t us;
    uintptr_t prompt, move;
    uint64_t moveHash;
    float el;
    uint16_t id;
    uint8_t type, res;
    uintptr_t owner; // 0 = 奎托斯；否则是 ctx[0]（别的上下文）
    uintptr_t ret;   // 调用者返回地址
};
constexpr uint32_t kQRing = 1 << 14;
QRec g_qRing[kQRing];
std::atomic<uint32_t> g_qHead{0};
std::atomic<bool> g_qteLog{true};

void PushQte(uintptr_t ctx, const uint8_t* prompt, uint64_t r, uintptr_t ret)
{
    const uintptr_t h = g_hero.load(std::memory_order_relaxed);
    if (!h || !ctx)
        return;
    const uintptr_t c0 = Rd<uintptr_t>(ctx);
    uintptr_t owner = 0;
    if (c0 != h) {
        // 别的上下文：只要 L3/R3/L3+R3 类，或者按键类结果成立的
        const uint16_t id = Rd<uint16_t>(reinterpret_cast<uintptr_t>(prompt) + 0xe8);
        const bool btn = id >= 1 && id <= 0x16;
        if (!(id == 0x15 || id == 0x0A || id == 0x07 || (btn && (r & 0xFF))))
            return;
        owner = c0 ? c0 : 1;
    }
    const uintptr_t ms = Rd<uintptr_t>(h + 0x3a70);
    QRec& e = g_qRing[g_qHead.fetch_add(1, std::memory_order_relaxed) & (kQRing - 1)];
    e.owner = owner;
    e.ret = ret;
    e.us = NowUs();
    e.prompt = reinterpret_cast<uintptr_t>(prompt);
    e.move = ms ? Rd<uintptr_t>(ms + 0x118) : 0;
    e.moveHash = ms ? Rd<uint64_t>(ms + 0x128) : 0;
    e.el = ms ? Rd<float>(ms + 0xd4) : 0.f;
    e.id = Rd<uint16_t>(e.prompt + 0xe8);
    e.type = Rd<uint8_t>(e.prompt + 0xef);
    e.res = static_cast<uint8_t>(r & 0xFF);
}

// ---- 2. 按键条件判定 0x87ac70：按住期间 L1（动作ID 0x05）类型 1/7 一律成立 ----
using CondFn = uint64_t(__fastcall*)(void* ctx, uint8_t* prompt, void* out);
// mov r11,rsp / push rsi / push r12 / push r13 / sub rsp,0x60
constexpr uint8_t kCondBytes[12] = {0x4C, 0x8B, 0xDC, 0x56, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x60};
uint64_t __fastcall CondDetour(void*, uint8_t*, void*);
Hook g_cond{"按键条件", 0x87ac70, kCondBytes, sizeof(kCondBytes), 6, reinterpret_cast<void*>(&CondDetour)};

uint64_t __fastcall CondDetour(void* ctx, uint8_t* prompt, void* out)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    PressPadButtons(reinterpret_cast<uintptr_t>(ctx));
    __try {
        ApplyVirtualPadAtCond(reinterpret_cast<uintptr_t>(ctx));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    uint64_t r = reinterpret_cast<CondFn>(g_cond.tramp)(ctx, prompt, out);
    if (prompt && g_qteLog.load(std::memory_order_relaxed))
        PushQte(reinterpret_cast<uintptr_t>(ctx), prompt, r, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    SampleDefense();
    if (prompt && *reinterpret_cast<uint16_t*>(prompt + 0xe8) == 0x01 && prompt[0xef] == 1) { // × 按下
        const uint8_t now = static_cast<uint8_t>(r & 0xFF) != 0;
        if (g_evadeBtn.exchange(now) != now)
            PushDef(now ? 1 : 2);
    }
    const int64_t ps = g_pressStart.load(std::memory_order_relaxed);
    if (ps && prompt && NowUs() - ps < kHoldUs) {
        const uint16_t id = *reinterpret_cast<uint16_t*>(prompt + 0xe8);
        const uint8_t type = prompt[0xef];
        if (id == 0x05 && (type == 1 || type == 7)) {
            g_condSeen.fetch_add(1, std::memory_order_relaxed);
            const bool release = g_releaseFirst.load(std::memory_order_relaxed) && NowUs() - ps < kReleaseUs;
            const uint64_t want = release ? 0 : 1;
            if ((r & 0xFF) != want) {
                r = (r & ~0xFFull) | want;
                g_forced.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else if (prompt && *reinterpret_cast<uint16_t*>(prompt + 0xe8) == 0x05 && prompt[0xef] == 7) {
        g_l1Natural.store((r & 0xFF) != 0, std::memory_order_relaxed);
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

// ---- 3. 格挡判定 0x79f0c0：只记录奎托斯挨打的结果 ----
using BlockFn = uint64_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uint64_t, uint64_t, uint64_t, uint8_t*);
constexpr uint8_t kBlockBytes[8] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49};
uint64_t __fastcall BlockDetour(uintptr_t, uintptr_t, uintptr_t, uint64_t, uint64_t, uint64_t, uint8_t*);
std::atomic<uintptr_t> g_projAtk{0}; // 最近一次被强制挡开的飞行物：攻击者 / 命中标志 / 时间
std::atomic<uint64_t> g_projF{0};
std::atomic<int64_t> g_projUs{0};
std::atomic<uintptr_t> g_projSeenAtk{0}; // 最近一次飞行物那条路的格挡判定（不管改没改结果），碰撞入口靠它认出「这一下是飞行物」
std::atomic<uint64_t> g_projSeenF{0};
std::atomic<int64_t> g_projSeenUs{0};
std::atomic<uint64_t> g_projNoF{0}; // 飞行物扫掠里看到的、盾接不了的那颗（攻击定义带 FUnblockable）的命中标志 / 时间：不强制挡开
std::atomic<int64_t> g_projNoUs{0};
Hook g_block{"格挡判定", 0x79f0c0, kBlockBytes, sizeof(kBlockBytes), 7, reinterpret_cast<void*>(&BlockDetour)};

uint64_t __fastcall BlockDetour(uintptr_t dc, uintptr_t attacker, uintptr_t victim, uint64_t f, uint64_t p5, uint64_t p6,
    uint8_t* out)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    // 背后来的攻击：弹反体 +0x08 挂着附加条件表（0x7c33a0 判，背后的会不过）。
    // 奎托斯在弹反窗口里时，这次判定先把弹反体的条件指针清零，判完原样还原 → 背后也能弹反。
    uintptr_t vols[kSetSlots], conds[kSetSlots];
    int nv = 0;
    IsHero(attacker); // 奎托斯砍敌人时也认出他，好提前开始记防御组
    const bool hero = IsHero(victim);
    if (hero && g_anyAngle.load(std::memory_order_relaxed)) {
        const uintptr_t D = Rd<uintptr_t>(victim + 0x3a70);
        const int nSets = D ? Rd<int>(D + 0x48) : 0;
        for (int s = 0; s < nSets && s < 4; ++s) {
            const uintptr_t S = Rd<uintptr_t>(D + 8 + s * 8);
            const int n = S ? Rd<int>(S + 0x80) : 0;
            for (int i = 0; i < n && i < kSetSlots && nv < kSetSlots; ++i) {
                const uintptr_t v = Rd<uintptr_t>(S + i * 8);
                const uintptr_t c = v ? Rd<uintptr_t>(v + 8) : 0;
                if (!c || !(Rd<float>(v + 0x40) > 0.f))
                    continue;
                __try {
                    *reinterpret_cast<uintptr_t*>(v + 8) = 0;
                    vols[nv] = v;
                    conds[nv++] = c;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
    // p6 = 命中记录 +0xd0：0x1401dcac0 按攻击者位置查出的「打在受击者哪个区域」。弹反体 +0x20 只认 011CBBEB9CE82EB8，
    // 但不同敌人/招式会算出别的区域（实测正面 000042500EBF0338）→ 弹反窗口里也对不上。游戏规则里 p6 = 0 表示不限区域，
    // 所以奎托斯在弹反窗口里时，这次判定传 p6 = 0。
    uint64_t p6Use = p6;
    if (hero && g_anyZone.load(std::memory_order_relaxed) && p6 && InParryWindow(victim))
        p6Use = 0;
    uint64_t r = reinterpret_cast<BlockFn>(g_block.tramp)(dc, attacker, victim, f, p5, p6Use, out);
    const uint8_t origR = static_cast<uint8_t>(r), origOut = out ? *out : 0xFF;
    // 飞行物（远程）：不走近战碰撞入口 0x8c9e30，而是 0x818e10 直接调格挡判定，结果写给飞行物处理（挡住/弹开 → 反弹）。
    // 正面区域（p6 = 011CBBEB9CE82EB8）、不是不可格挡（命中标志高 16 位没有 4）、你没自己按着 L1 → 判定改成「挡住 + 弹开」并按 L1
    // v6.14：同一颗飞行物同一帧还会再判一次（实测 p5=…00000082，调用者不在 0x818e10 里）→ 100 ms 内同攻击者、同命中标志的也强制
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_exe;
    const bool projCall = ret >= 0x818e10 && ret < 0x8192be;
    const bool projFollow = !projCall && attacker && attacker == g_projAtk.load(std::memory_order_relaxed) &&
        f == g_projF.load(std::memory_order_relaxed) && NowUs() - g_projUs.load(std::memory_order_relaxed) < 100'000;
    if (hero && projCall) {
        g_projSeenAtk.store(attacker, std::memory_order_relaxed);
        g_projSeenF.store(f, std::memory_order_relaxed);
        g_projSeenUs.store(NowUs(), std::memory_order_relaxed);
    }
    // v6.23：不再看命中标志高 16 位的 0004（见碰撞入口的说明）。盾接不了 = 等级位不在盾掩码里（NoShield），
    // 或者这颗飞行物的攻击定义带 FUnblockable（扫掠钩子里读到的，300 ms 内同命中标志）
    const bool projHardNo = NoShield(f) || (f == g_projNoF.load(std::memory_order_relaxed) &&
                                               NowUs() - g_projNoUs.load(std::memory_order_relaxed) < 300'000);
    if (hero && (projCall || projFollow) && g_parryOn.load(std::memory_order_relaxed) &&
        (p6 == 0x011CBBEB9CE82EB8ull || projFollow) && !projHardNo && !((r & 0xFF) && out && *out)) {
        if (projCall) {
            g_projAtk.store(attacker, std::memory_order_relaxed);
            g_projF.store(f, std::memory_order_relaxed);
            g_projUs.store(NowUs(), std::memory_order_relaxed);
        }
        r = (r & ~0xFFull) | 1;
        if (out)
            *out = 1;
        const int64_t now = NowUs();
        const int64_t ps = g_pressStart.load();
        if (!g_l1Natural.load(std::memory_order_relaxed) && (ps == 0 || now - ps > kRearmUs)) {
            g_releaseFirst.store(false);
            g_pressStart.store(now);
        }
        Push(kProjParry, attacker, victim, origR, origOut, 0, f);
    }
    for (int i = 0; i < nv; ++i)
        __try {
            *reinterpret_cast<uintptr_t*>(vols[i] + 8) = conds[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    if (IsHero(attacker) && victim && victim == g_passAttacker.load() && NowUs() - g_passUs.load() < 600'000) {
        g_passAttacker.store(0);
        g_parryOk.fetch_add(1);
        Push(kShove, attacker, victim, static_cast<uint8_t>(r), 0, 0, f, p5, p6);
    }
    if (hero)
        Push(kBlock, attacker, victim, static_cast<uint8_t>(r), out ? *out : 0xFF, static_cast<uint8_t>(nv), f, p5, p6,
            static_cast<uint32_t>(ret));
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

// ---- 飞行物预判弹反（v6.15）----
// v6.14 实测：飞行物碰到时才把判定改成挡开 → 不掉血，但飞行物不会被打回去（碰到那一刻奎托斯还没有弹反体，匹配防御体=0）。
// 0x8182e0(飞行物, from, to, 半径, …) 是飞行物每帧的扫掠检测（这一帧 from → to）。顺着这条线往前推，
// 离奎托斯胸口够近、在正面、剩余时间 ≤ proj_lead_ms → 现在就按 L1，让飞行物真碰到时游戏自己判弹反。
constexpr uint8_t kSweepBytes[8] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x57};
bool __fastcall SweepDetour(uintptr_t, const float*, const float*, float, uint64_t, uint64_t*);
Hook g_sweep{"飞行物扫掠", 0x8182e0, kSweepBytes, sizeof(kSweepBytes), 8, reinterpret_cast<void*>(&SweepDetour)};
using SweepFn = bool(__fastcall*)(uintptr_t, const float*, const float*, float, uint64_t, uint64_t*);
std::atomic<bool> g_projPre{true};   // ini proj_pre
std::atomic<int> g_projLeadMs{160};  // ini proj_lead_ms
std::atomic<bool> g_projEvade{true}; // ini proj_evade：盾接不了的 / 背后来的飞行物，到点提前侧闪（v6.23）
std::atomic<int> g_projEvadeMs{300}; // ini proj_evade_ms
std::atomic<bool> g_projDiag{false}; // ini proj_diag：逐条记「· 扫掠」诊断行（调轨迹预判时才开）
std::atomic<uint32_t> g_sweepCalls{0};
struct ProjSlot {
    uintptr_t proj;
    int64_t lastUs, firstUs, pressedUs;
    float last[3], first[3];
};
ProjSlot g_projSlots[32];
struct SweepRec {
    int64_t us;
    uintptr_t proj;
    float a[3], b[3], hp[3], radius, t, miss;
    int64_t dt;
    uint32_t pflags;
};
constexpr uint32_t kSweepRing = 512;
SweepRec g_sweepRing[kSweepRing];
std::atomic<uint32_t> g_sweepHead{0};
std::atomic<int64_t> g_sweepLogUs{0};

// 飞行物的发射者：proj+0xf0 是对象句柄，查表 0x1422a98c0..0x1422a9ac0（同 0x1404ef0b0，只读）→ 游戏对象
uintptr_t ProjOwnerGo(uintptr_t proj)
{
    const uint64_t h = Rd<uint64_t>(proj + 0xf0);
    if (!(h & 1) || (h & 0xfc000000000ull))
        return 0;
    const int id = static_cast<int>((h >> 1) & 0xffff);
    const uint32_t idx = static_cast<uint32_t>((h >> 0x12) & 0xfffff);
    const uintptr_t off = ((h >> 0x11) & 1) ? 0x20 : 8;
    for (uintptr_t a = g_exe + 0x22a98c0; a < g_exe + 0x22a9ac0; a += 8) {
        const uintptr_t t = Rd<uintptr_t>(a);
        if (!t || Rd<int>(t) != id)
            continue;
        if (Rd<uint32_t>(t + off + 0x10) <= idx)
            return 0;
        return Rd<uintptr_t>(Rd<uintptr_t>(t + off) + idx * 8);
    }
    return 0;
}

void GoName(uintptr_t go, char (&n)[24])
{
    memset(n, 0, sizeof(n));
    const uintptr_t str = go ? Rd<uintptr_t>(go + 0x38) : 0;
    if (!str || !SafeRead(str, n, sizeof(n) - 1))
        strcpy_s(n, "?");
    n[sizeof(n) - 1] = 0;
}

// 自己人（奎托斯、阿特柔斯）射的不算：v6.18 实测阿特柔斯的箭从奎托斯身边飞过（偏离 0.1~1.1 m）会误按 L1 → 抽搐
bool IsAllyGo(uintptr_t go)
{
    if (!go)
        return false;
    const uintptr_t hero = g_hero.load(std::memory_order_relaxed);
    if (hero && go == Rd<uintptr_t>(hero + 8))
        return true;
    char n[24];
    GoName(go, n);
    return strncmp(n, "gohero", 6) == 0 || strncmp(n, "goson", 5) == 0 || strncmp(n, "goboy", 5) == 0 ||
        strncmp(n, "goatreus", 8) == 0;
}

std::atomic<uintptr_t> g_lastOwnerGo{0};

void ProjPredict(uintptr_t proj, const float* from, const float* to, float radius)
{
    const uintptr_t hero = g_hero.load(std::memory_order_relaxed);
    if (!hero || !proj || !from || !to)
        return;
    const uintptr_t data = Rd<uintptr_t>(proj + 0x198);
    if (!data)
        return;
    const uint64_t f = Rd<uint64_t>(data + 0x98);
    if (Rd<uint32_t>(data + 0x1a8) & 0x800000) // FIgnoreCreature：不打生物
        return;
    // v6.23：飞行物一生成，它的攻击定义（data+0x50 内嵌的 Collision）就在手里，不用等碰到才知道盾接不接得住。
    // 接不了 = 攻击定义带 FUnblockable（+0x94 & 4），或者等级位不在盾的掩码里（NoShield）。命中标志高 16 位的 0004 不算。
    // 接不了的以前直接跳过（只能碰到才闪），现在照样跟踪弹道，到点提前侧闪。
    const bool hard = (Rd<uint32_t>(data + 0x94) & 4) != 0 || NoShield(f);
    float a[3], b[3];
    if (!SafeRead(reinterpret_cast<uintptr_t>(from), a, sizeof(a)) || !SafeRead(reinterpret_cast<uintptr_t>(to), b, sizeof(b)))
        return;
    const uintptr_t hg = Rd<uintptr_t>(hero + 8);
    float hp[3], L[12];
    if (!WorldPos(hg, hp) || !SafeRead(hg + 0x60, L, sizeof(L)))
        return;
    hp[1] += 1.0f; // 胸口高度
    const float v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const float r[3] = {hp[0] - a[0], hp[1] - a[1], hp[2] - a[2]};
    const float t = vv > 1e-6f ? (r[0] * v[0] + r[1] * v[1] + r[2] * v[2]) / vv : -1.f; // 还要走几帧到离奎托斯最近的点
    if (g_projDiag.load(std::memory_order_relaxed)) { // 诊断（v6.16，ini proj_diag，默认关）：离奎托斯 20 m 内的扫掠，每 20 ms 最多记一条
        const float d2 = r[0] * r[0] + r[2] * r[2];
        const int64_t nowD = NowUs();
        if (d2 < 400.f && nowD - g_sweepLogUs.load(std::memory_order_relaxed) > 20'000) {
            g_sweepLogUs.store(nowD, std::memory_order_relaxed);
            SweepRec& q = g_sweepRing[g_sweepHead.fetch_add(1) & (kSweepRing - 1)];
            q.us = nowD;
            q.proj = proj;
            memcpy(q.a, a, sizeof(a));
            memcpy(q.b, b, sizeof(b));
            memcpy(q.hp, hp, sizeof(hp));
            q.radius = radius;
            q.t = t;
            const float cx = a[0] + v[0] * t - hp[0], cy = a[1] + v[1] * t - hp[1], cz = a[2] + v[2] * t - hp[2];
            q.miss = t > 0.f ? sqrtf(cx * cx + cy * cy + cz * cz) : -1.f;
            q.dt = 0;
            q.pflags = Rd<uint32_t>(data + 0x1a8);
        }
    }
    if (!(vv > 1e-6f))
        return;
    (void)t;
    // v6.17：to - from 只是这一帧的一小段（实测约 0.1 m），一帧里还会扫好几次，不能当速度用。
    // 改成按同一颗飞行物「第一次看到 → 现在」的位移和时间算速度和方向；对象会被复用，跳回去 >3 m 或断档 >300 ms 算新的一颗。
    const int64_t now = NowUs();
    ProjSlot* s = nullptr;
    ProjSlot* old = &g_projSlots[0];
    for (auto& x : g_projSlots) {
        if (x.proj == proj) {
            s = &x;
            break;
        }
        if (x.lastUs < old->lastUs)
            old = &x;
    }
    auto reset = [&](ProjSlot* q) {
        q->proj = proj;
        q->firstUs = q->lastUs = now;
        q->pressedUs = 0;
        memcpy(q->first, a, sizeof(a));
        memcpy(q->last, a, sizeof(a));
    };
    if (!s) {
        reset(old);
        return;
    }
    {
        const float jx = a[0] - s->last[0], jy = a[1] - s->last[1], jz = a[2] - s->last[2];
        if (now - s->lastUs > 300'000 || jx * jx + jy * jy + jz * jz > 9.f) {
            reset(s);
            return;
        }
    }
    s->lastUs = now;
    memcpy(s->last, a, sizeof(a));
    const int64_t el = now - s->firstUs;
    if (el < 30'000)
        return;
    const float m[3] = {a[0] - s->first[0], a[1] - s->first[1], a[2] - s->first[2]};
    const float mlen = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    if (!(mlen > 0.05f))
        return;
    const float speed = mlen / (static_cast<float>(el) * 1e-6f); // 米/秒
    const float u[3] = {m[0] / mlen, m[1] / mlen, m[2] / mlen};
    const float along = r[0] * u[0] + r[1] * u[1] + r[2] * u[2]; // 顺着飞行方向离奎托斯最近点还有多远
    if (!(along > 0.f))
        return;
    const float c[3] = {a[0] + u[0] * along - hp[0], a[1] + u[1] * along - hp[1], a[2] + u[2] * along - hp[2]};
    const float miss = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(miss < radius + 1.2f))
        return;
    const float hitDist = along - 0.4f; // 打到身体表面，比胸口中心早约 0.4 m
    const int64_t tti = hitDist > 0.f ? static_cast<int64_t>(hitDist / speed * 1e6f) : 0;
    if (hard && tti < 300'000) { // 告诉格挡判定：这颗盾接不了，别强制挡开
        g_projNoF.store(f, std::memory_order_relaxed);
        g_projNoUs.store(now, std::memory_order_relaxed);
    }
    const int64_t leadParry = static_cast<int64_t>(g_projLeadMs.load(std::memory_order_relaxed)) * 1000;
    const int64_t leadEvade = static_cast<int64_t>(g_projEvadeMs.load(std::memory_order_relaxed)) * 1000;
    if (s->pressedUs || tti > (std::max)(leadParry, leadEvade))
        return;
    const uintptr_t ownerGo = ProjOwnerGo(proj);
    const bool ally = IsAllyGo(ownerGo);
    const float dx = a[0] - hp[0], dz = a[2] - hp[2];
    const float deg = atan2f(-(dx * L[0] + dz * L[2]), -(dx * L[8] + dz * L[10])) * 57.29578f;
    const bool front = InParryZone(hero, deg);
    // 该闪的：盾接不了，或者从背后来（弹反区域外）。闪比按 L1 动静大，所以弹道要更贴身才算（偏离 < 半径 + 0.8 m）
    const bool evadeIt = !ally && (hard || !front) && miss < radius + 0.8f && g_evadeOn.load(std::memory_order_relaxed) &&
                         g_projEvade.load(std::memory_order_relaxed);
    if (tti > (evadeIt ? leadEvade : leadParry))
        return;
    s->pressedUs = now;
    g_lastOwnerGo.store(ownerGo, std::memory_order_relaxed);
    bool pressed = false;
    if (evadeIt) {
        const int64_t es = g_evadeStart.load();
        if (!GamePaused() && (es == 0 || now - es > kEvadeRearmUs)) {
            // 侧闪离开弹道：c = 弹道上离胸口最近的点 - 胸口，往反方向走；几乎正对着（水平偏离 < 0.15 m）就取飞行方向的垂直方向
            float ex = -c[0], ez = -c[2];
            if (ex * ex + ez * ez < 0.15f * 0.15f) {
                ex = -u[2];
                ez = u[0];
            }
            const float el2 = sqrtf(ex * ex + ez * ez);
            g_evadeDirX.store(el2 > 0.f ? ex / el2 : 0.f);
            g_evadeDirZ.store(el2 > 0.f ? ez / el2 : 0.f);
            g_evAttacker.store(ownerGo ? Rd<uintptr_t>(ownerGo + 0x220) : 0); // 碰撞入口里攻击者 = 发射者的生物
            g_evLast.store(now);
            g_predUntil.store(now + tti + 600'000); // 闪了还是碰到 → 碰撞入口按「预判闪避」兜底吞掉并记一行
            g_evadeRollNow.store(false);
            g_evadeStart.store(now);
            pressed = true;
        }
    } else if (!ally && !hard && front && g_parryOn.load(std::memory_order_relaxed) && !g_l1Natural.load(std::memory_order_relaxed)) {
        const int64_t ps = g_pressStart.load();
        if (ps == 0 || now - ps > kRearmUs) {
            g_releaseFirst.store(false);
            g_pressStart.store(now);
            pressed = true;
        }
    }
    const float distXZ = sqrtf(dx * dx + dz * dz);
    Push(kProjPre, 0, ownerGo, pressed, front, ally, f,
        (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int>(deg * 10.f))) << 32) | static_cast<uint32_t>(tti),
        (static_cast<uint64_t>(static_cast<uint32_t>(distXZ * 100.f)) << 32) | static_cast<uint32_t>(miss * 1000.f),
        (evadeIt ? 1u : 0u) | (hard ? 2u : 0u));
}

bool __fastcall SweepDetour(uintptr_t proj, const float* from, const float* to, float radius, uint64_t p5, uint64_t* p6)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    g_sweepCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_projPre.load(std::memory_order_relaxed))
        ProjPredict(proj, from, to, radius);
    const bool r = reinterpret_cast<SweepFn>(g_sweep.tramp)(proj, from, to, radius, p5, p6);
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

const char* Name(uintptr_t creature, char (&buf)[32])
{
    buf[0] = 0;
    const uintptr_t go = Rd<uintptr_t>(creature + 8);
    const uintptr_t str = go ? Rd<uintptr_t>(go + 0x38) : 0;
    if (!str || !SafeRead(str, buf, 31))
        strcpy_s(buf, "?");
    buf[31] = 0;
    return buf;
}

// ---- 5. 连按 / 定时单按 QTE 自动完成（v6.20）----
// 游戏的连按是招式脚本动作 SCR_RepeatPress（按 RTTI 找到虚表 GoW+0xe2f1c8，[3] 每帧更新 = 0x2b67c0），
// 定时单按是 SCR_TimedPress（虚表 GoW+0xe2f120，[3] = 0x2b7940）。两个更新函数自己带「自动成功」路径：
// 全局字节 GoW+0x27d2d34 非 0 时
//   · 连按：按键判定 0x987150 直接算按下，进度按每秒 5 下的速度自己涨到满（记录标志第 9 位的那种除外，游戏自己不让自动）；
//     但「重複按按鈕」设成按住（GoW+0x22E7944 ≠ 0）时走的是「按住」分支、不看这个字节，所以调用期间要一起临时拨成 0；
//   · 定时单按：当帧就判成功。
// 这个字节平时是 0。全局置 1 会连带改别处（0x87b770 / 0x8e9360 / 0x98d800 把输入上下文 +0x498、+0x4a4 换成 100.0），
// 所以只在这两个更新函数执行期间置 1，出来就还原。
// 挂法：换虚表里的函数指针（.rdata，8 字节对齐，原子写），不改代码段。
struct VtHook {
    const char* name;
    uintptr_t slotRva, expectRva;
    void* detour;
    uintptr_t orig = 0;
    bool installed = false;
};

bool InstallVt(VtHook& h)
{
    auto* slot = reinterpret_cast<uint8_t*>(g_exe + h.slotRva);
    h.orig = g_exe + h.expectRva;
    if ((reinterpret_cast<uintptr_t>(slot) & 7) != 0 || Rd<uintptr_t>(reinterpret_cast<uintptr_t>(slot)) != h.orig) {
        Log("[%s] 虚表项对不上，不装", h.name);
        return false;
    }
    if (!SwapQword(slot, h.orig, reinterpret_cast<uint64_t>(h.detour))) {
        Log("[%s] 虚表项写入失败", h.name);
        return false;
    }
    h.installed = true;
    Log("[%s] 虚表项已换：%p", h.name, slot);
    return true;
}

void RemoveVt(VtHook& h)
{
    if (!h.installed)
        return;
    auto* slot = reinterpret_cast<uint8_t*>(g_exe + h.slotRva);
    Log("[%s] %s", h.name, SwapQword(slot, reinterpret_cast<uint64_t>(h.detour), h.orig) ? "已恢复虚表项" : "恢复虚表项失败！");
    h.installed = false;
}

using ScrUpdateFn = void(__fastcall*)(void* self, uintptr_t creature);
constexpr uintptr_t kAutoMinigameRva = 0x27d2d34; // 游戏自带的「小游戏自动成功」字节
constexpr uintptr_t kRepeatChoiceRva = 0x22e7944; // 重複按按鈕：0 連續輕碰，非 0 按住
std::atomic<bool> g_qteMash{true};   // ini qte_mash
std::atomic<uintptr_t> g_mashSelf{0}; // 最近一次在跑的连按动作对象 / 时刻 / (记录标志 << 16 | 按键类型)
std::atomic<int64_t> g_mashUs{0};
std::atomic<uint32_t> g_mashInfo{0};
std::atomic<uint32_t> g_mashTicks{0}, g_timedTicks{0};
CRITICAL_SECTION g_autoCs; // 两个全局是「临时改、出来还原」，串行化，免得两个线程互相把临时值当成原值存下来
int g_autoDepth = 0;
int16_t g_holdSaved = 0;

void CallWithAutoMinigame(uintptr_t orig, void* self, uintptr_t creature)
{
    auto* autoFlag = reinterpret_cast<volatile uint8_t*>(g_exe + kAutoMinigameRva);
    auto* holdMode = reinterpret_cast<volatile int16_t*>(g_exe + kRepeatChoiceRva);
    EnterCriticalSection(&g_autoCs);
    if (g_autoDepth++ == 0)
        g_holdSaved = *holdMode;
    *autoFlag = 1;
    *holdMode = 0;
    __try {
        reinterpret_cast<ScrUpdateFn>(orig)(self, creature);
    } __finally {
        if (--g_autoDepth == 0) {
            *autoFlag = 0;
            *holdMode = g_holdSaved;
        }
        LeaveCriticalSection(&g_autoCs);
    }
}

void __fastcall RepeatPressDetour(void*, uintptr_t);
void __fastcall TimedPressDetour(void*, uintptr_t);
VtHook g_vtRepeat{"连按 QTE", 0xe2f1c8 + 0x18, 0x2b67c0, reinterpret_cast<void*>(&RepeatPressDetour)};
VtHook g_vtTimed{"定时单按 QTE", 0xe2f120 + 0x18, 0x2b7940, reinterpret_cast<void*>(&TimedPressDetour)};

void __fastcall RepeatPressDetour(void* self, uintptr_t creature)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    if (g_qteMash.load(std::memory_order_relaxed)) {
        // 动作对象 +0x20 = 连按状态槽，槽 +0x20 = 记录参数：+0 标志（第 9 位 = 不许自动），+0x40 按键类型（eMashButtons）
        const uintptr_t slot = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(self) + 0x20);
        const uintptr_t rec = slot ? Rd<uintptr_t>(slot + 0x20) : 0;
        const uint8_t btn = rec ? Rd<uint8_t>(rec + 0x40) : 0xFF;
        g_mashInfo.store(rec ? (static_cast<uint32_t>(Rd<uint16_t>(rec)) << 16) | btn : 0xFF, std::memory_order_relaxed);
        g_mashSelf.store(reinterpret_cast<uintptr_t>(self), std::memory_order_relaxed);
        g_mashUs.store(NowUs(), std::memory_order_relaxed);
        g_mashTicks.fetch_add(1, std::memory_order_relaxed);
        // 只自动按键类（0..5 = △ ○ × □ R1 L1）：这条路径逐行核对过。扳机模拟量（6..8）在自动字节下读数恒为 1.0、
        // 只涨第一下就停，转摇杆 / L1R1 交替（9..12）根本不看自动字节 —— 这几类原样交给你手按，免得卡住
        if (btn <= 5)
            CallWithAutoMinigame(g_vtRepeat.orig, self, creature);
        else
            reinterpret_cast<ScrUpdateFn>(g_vtRepeat.orig)(self, creature);
    } else {
        reinterpret_cast<ScrUpdateFn>(g_vtRepeat.orig)(self, creature);
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
}

void __fastcall TimedPressDetour(void* self, uintptr_t creature)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    if (g_qteMash.load(std::memory_order_relaxed)) {
        g_timedTicks.fetch_add(1, std::memory_order_relaxed);
        CallWithAutoMinigame(g_vtTimed.orig, self, creature);
    } else {
        reinterpret_cast<ScrUpdateFn>(g_vtTimed.orig)(self, creature);
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
}

// 圆形按键图标 SCR_CircleIcon（敌人头上的 R3 这类提示）：只记录，不改任何东西。
// 开始函数 [2] = 0x25cd50(this, 生物)：记录 = *(*(*(this+8)+0xc8)+0x10)，+0x18 模式（0 招式这段期间显示 / 1 关掉 / 2 打开），
// +0x19 图标号；它把图标号写进生物 +0x4a40、开关写进 +0x4a44，然后跳到 [3]
struct IconRec {
    uintptr_t creature;
    int8_t icon;
    uint8_t mode;
};
constexpr uint32_t kIconRing = 64;
IconRec g_iconRing[kIconRing];
std::atomic<uint32_t> g_iconHead{0};

void __fastcall CircleIconDetour(void*, uintptr_t);
VtHook g_vtIcon{"按键图标（只记录）", 0xe26078 + 0x10, 0x25cd50, reinterpret_cast<void*>(&CircleIconDetour)};

void __fastcall CircleIconDetour(void* self, uintptr_t creature)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const uintptr_t a = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(self) + 8);
    const uintptr_t b = a ? Rd<uintptr_t>(a + 0xc8) : 0;
    if (const uintptr_t rec = b ? Rd<uintptr_t>(b + 0x10) : 0) {
        // 两个线程同时进来最多丢一条记录，只是日志，不加锁
        const uint32_t i = g_iconHead.load(std::memory_order_relaxed);
        g_iconRing[i & (kIconRing - 1)] = {creature, Rd<int8_t>(rec + 0x19), Rd<uint8_t>(rec + 0x18)};
        g_iconHead.store(i + 1, std::memory_order_release);
    }
    reinterpret_cast<ScrUpdateFn>(g_vtIcon.orig)(self, creature);
    g_inflight.fetch_sub(1, std::memory_order_release);
}

DWORD WINAPI Worker(void*)
{
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"gow4_autoparry.log");
    g_log = _wfsopen(path, L"ab", _SH_DENYNO);
    Log("==== 自动弹反实验 v6.23（飞行物：命中标志高位 0004 不再当红光，盾接得住的照样提前按 L1 弹回去；盾真接不了的 / 背后来的飞行物提前侧闪；招式预读（v6.22）：敌人一出招就从招式数据读出每段攻击的命中标志，红光 / 盾接不住的提前闪，含冲击波；日志加「预读」「预读核对」行；其余同 v6.21：盾掩码规则、完整命中标志、敌人定义；v6.20：游戏自己的玩家指针、连按/定时单按 QTE 自动完成、按键图标记录、飞行物提前按 L1 + 兜底挡开、红光闪避）加载 ====");
    InitializeCriticalSection(&g_autoCs);
    HANDLE unload = CreateEventW(nullptr, TRUE, FALSE, L"Local\\gow4_autoparry_unload");
    g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exe + reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe)->e_lfanew);
    bool ok = nt->OptionalHeader.SizeOfImage > 0x2000000;
    if (!ok)
        Log("宿主进程不是 GoW.exe，不装");
    // 先装只记录的，再装按键，最后装会改行为的碰撞入口；任何一个失败就全部不启用
    ok = ok && Install(g_block) && Install(g_cond) && Install(g_contact) && Install(g_poll);
    if (ok && !Install(g_sweep))
        Log("飞行物预判没装上，只用碰到时强制挡开");
    if (ok && !(InstallVt(g_vtRepeat) & InstallVt(g_vtTimed)))
        Log("QTE 自动完成没全装上（上面有具体是哪个），其余功能不受影响");
    if (ok)
        InstallVt(g_vtIcon); // 只记录用，装不上也无所谓

    static const char* const kKindText[] = {"吞掉并按下 L1", "继续吞", "放行（已在弹反窗口）", "放行（吞超时）",
        "放行（冷却中）", "格挡判定", "自动弹反关着，照常结算", "背后来的：吞掉并按 ×", "背后来的：继续吞（闪避中）", "背后来的：放行（吞超时）", "弹反成功", "不在可弹反区域（背后半圈），暂不处理", "你自己按着 L1，照常格挡", "预判闪避后还是碰到：吞掉兜底", "预判闪避后碰到：不吞，交给游戏判定", "飞行物打正面：判定改成挡开 + 按 L1", "飞行物预判"};
    wchar_t ini[MAX_PATH];
    GetModuleFileNameW(g_self, ini, MAX_PATH);
    if (wchar_t* slash = wcsrchr(ini, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"gow4_autoparry.ini");
    g_snaps = static_cast<Snap*>(VirtualAlloc(nullptr, sizeof(Snap) * kSnapRing, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FILE* snapFile = nullptr;
    {
        wchar_t sp[MAX_PATH];
        GetTempPathW(MAX_PATH, sp);
        wcscat_s(sp, L"gow4_ms_snap.bin");
        snapFile = _wfsopen(sp, L"ab", _SH_DENYNO);
    }
    uint32_t snapTail = g_snapHead.load();
    FILE* qf = nullptr;
    {
        wchar_t qp[MAX_PATH];
        GetTempPathW(MAX_PATH, qp);
        wcscat_s(qp, L"gow4_qte.log");
        qf = _wfsopen(qp, L"ab", _SH_DENYNO);
    }
    auto QLog = [&](const char* fmt, auto... args) {
        if (!qf)
            return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(qf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        fprintf(qf, fmt, args...);
        fputc(0x0A, qf);
    };
    QLog("==== QTE 记录器（v6.9）开始：重複按按鈕=%d 鑿子門=%d 格擋切換=%d 全局795B=%d ====",
        Rd<int16_t>(g_exe + 0x22E7944), Rd<uint8_t>(g_exe + 0x22E7930), Rd<uint8_t>(g_exe + 0x22E7959),
        Rd<uint8_t>(g_exe + 0x22E795B));
    struct QLast {
        uint16_t id;
        uint8_t type, res;
    };
    std::unordered_map<uintptr_t, QLast> qLast;
    uint32_t qTail = g_qHead.load();
    uint64_t qMoveHash = 0;
    uint64_t qFlushAt = 0;
    std::unordered_set<uintptr_t> seenVols;
    std::unordered_set<uintptr_t> seenDefs;
    std::unordered_set<uintptr_t> seenMoves;  // 预读结果已经记过日志的招式
    std::unordered_set<uint64_t> seenHitDefs; // 「预读对没对上」已经记过的（招式, 攻击定义）
    uint32_t tail = 0, defTail = 0;
    uint32_t lastForced = 0;
    uint64_t lastIni = 0;
    float lastMag = 0.f, lastDx = 0.f, lastDz = 0.f;
    int64_t seenEvade = 0, evadeCheckAt = 0;
    float evadeP0[3] = {};
    while (ok && WaitForSingleObject(unload, 50) != WAIT_OBJECT_0) {
        if (const int64_t es = g_evadeStart.load(); es && es != seenEvade) { // 每次闪避：记起点，0.4 s 后看移动了多少
            seenEvade = es;
            evadeCheckAt = es + 600'000;
            if (const uintptr_t h = g_hero.load())
                WorldPos(Rd<uintptr_t>(h + 8), evadeP0);
            Log("  ↷ %s：按 ×（%u 次）推摇杆 (%.2f, %.2f)", g_evadeRollNow.load() ? "翻滚" : "闪避", g_padWrites.load(),
                g_evadeDirX.load(), g_evadeDirZ.load());
        }
        // 自动测试序列（ini test_seq=1）：游戏连续运行 3 s 后，一前一后替你做 单下 + 几种节奏的翻滚，每次间隔 2.5 s
        {
            struct Step { int tap1, gap, tap2; };
            // gap=0 表示单下（按 tap1 毫秒）；每种做两次（一后一前）
            static const Step kSeq[] = {{200, 0, 0}, {200, 0, 0}, {80, 80, 120}, {80, 80, 120}, {100, 150, 150}, {100, 150, 150},
                {100, 0, 0}, {100, 0, 0}, {150, 100, 150}, {150, 100, 150}};
            static int seqIdx = -1;
            static int64_t runningSince = 0, nextAt = 0;
            const bool paused = GamePaused(); // 历史头 0xffff 或 0.3 s 没有奎托斯按键判定
            const int64_t now = NowUs();
            if (seqIdx < 0 && GetPrivateProfileIntW(L"autoparry", L"test_seq", 0, ini) != 0) {
                WritePrivateProfileStringW(L"autoparry", L"test_seq", L"0", ini);
                seqIdx = 0;
                runningSince = 0;
                Log("自动测试：已就绪，等游戏连续运行 3 s 后开始（共 %d 次）", static_cast<int>(std::size(kSeq)));
            }
            g_seqRunning = seqIdx >= 0;
            if (seqIdx >= 0) {
                if (paused) {
                    runningSince = 0;
                } else if (!runningSince) {
                    runningSince = now;
                    nextAt = now + 3'000'000;
                }
                const uintptr_t h = g_hero.load();
                if (runningSince && now >= nextAt && h) {
                    const Step& st = kSeq[seqIdx];
                    float L[12] = {};
                    SafeRead(Rd<uintptr_t>(h + 8) + 0x60, L, sizeof(L));
                    const float bl = sqrtf(L[8] * L[8] + L[10] * L[10]);
                    const float sg = (seqIdx % 2 == 0) ? 1.f : -1.f; // 偶数次往身后、奇数次往身前，来回不跑远
                    g_evadeDirX.store(bl > 0.f ? sg * L[8] / bl : 0.f);
                    g_evadeDirZ.store(bl > 0.f ? sg * L[10] / bl : 0.f);
                    g_evadeStick.store(true);
                    const bool roll = st.gap > 0;
                    g_rollTap1Us.store(st.tap1 * 1000);
                    g_rollGapUs.store(st.gap * 1000);
                    g_rollTap2Us.store(st.tap2 * 1000);
                    if (!roll)
                        g_evadeTapUs.store(st.tap1 * 1000);
                    g_evadeRollNow.store(roll);
                    const uintptr_t hms = Rd<uintptr_t>(h + 0x3a70);
                    Log("自动测试 %d/%d：%s 按 %d ms%s（往奎托斯%s；按之前他在招式 %016llX 已走 %.3f s）", seqIdx + 1,
                        static_cast<int>(std::size(kSeq)), roll ? "连按两下" : "单下", st.tap1, roll ? " / 松 / 再按" : "",
                        sg > 0.f ? "身后" : "身前", static_cast<unsigned long long>(hms ? Rd<uint64_t>(hms + 0x128) : 0),
                        hms ? Rd<float>(hms + 0xd4) : -1.f);
                    if (roll)
                        Log("  节奏：按 %d ms / 松 %d ms / 再按 %d ms", st.tap1, st.gap, st.tap2);
                    g_evadeStart.store(now);
                    nextAt = now + 3'500'000;
                    if (++seqIdx >= static_cast<int>(std::size(kSeq))) {
                        seqIdx = -1;
                        lastIni = 0; // 下一轮重读 ini，翻滚节奏恢复成配置值
                    }
                }
            }
        }
        if (evadeCheckAt && NowUs() >= evadeCheckAt) {
            evadeCheckAt = 0;
            float p[3] = {};
            if (const uintptr_t h = g_hero.load(); h && WorldPos(Rd<uintptr_t>(h + 8), p)) {
                const float dx = p[0] - evadeP0[0], dz = p[2] - evadeP0[2];
                const float along = dx * g_evadeDirX.load() + dz * g_evadeDirZ.load();
                Log("  ↷ 闪避结果：0.6 s 内移动 %.2f m，其中朝闪避方向 %.2f m（输入轮询写入 %u 次，按键判定时写入 %u 次）",
                    sqrtf(dx * dx + dz * dz), along, g_pollCalls.load(), g_condWrites.load());
            }
        }
        for (; snapFile && snapTail != g_snapHead.load(); ++snapTail) { // 快照落盘：每条 = 头(kind,us,creature,ms,anim) + 两块原始字节
            Snap& sn = g_snaps[snapTail % kSnapRing];
            if (sn.seq.load(std::memory_order_acquire) != snapTail + 1)
                break; // 还没写完，下一轮再取
            const uint64_t hdr[5] = {sn.kind, static_cast<uint64_t>(sn.us), sn.creature, sn.ms, sn.anim};
            fwrite(hdr, sizeof(hdr), 1, snapFile);
            fwrite(sn.msBytes, kSnapMs, 1, snapFile);
            fwrite(sn.animBytes, kSnapAnim, 1, snapFile);
        }
        if (snapFile)
            fflush(snapFile);
        for (static uint32_t hmTail = 0; hmTail != g_hmHead.load(); ++hmTail) {
            const HeroMoveRec& r = g_hmRing[hmTail & (kHmRing - 1)];
            Log("  ◇ 奎托斯换招：%s %lld ms → 招式 %p 哈希 %016llX 时长 %.3f s（进入时已走 %.3f s；当时 × %s）",
                r.manual ? "你按 × 后" : "替你闪避后", r.sinceEvade / 1000, reinterpret_cast<void*>(r.move), static_cast<unsigned long long>(r.hash), r.dur, r.elapsed,
                r.phase == 1 ? "按着" : (r.phase == 2 ? "松开(翻滚间隙)" : "已松开"));
        }
        for (static uint32_t warnTail = 0; warnTail != g_warnHead.load(); ++warnTail) {
            const WarnRec& r = g_warnRing[warnTail & (kWarnRing - 1)];
            char an[32], sg[200] = "";
            if (r.what == 1) {
                if (r.hasSeg)
                    FmtSeg(sg, sizeof(sg), r.seg, r.dur);
                Log("  ⇒ 预判闪避：%s 还有 %.0f ms 出伤（本招已进行 %.3f s / Attacking 标签到 %.3f s，速率 %.2f；方位 %.0f° 距离 %.1fm）%s%s",
                    Name(r.creature, an), r.remain * 1000.f, r.elapsed, r.tagEnd, r.rate, r.deg, r.dist, r.hasSeg ? " 依据预读：" : "", sg);
            } else if (r.start) {
                Log("◆ 出招预警开始 %s%s（方位 %.0f° 距离 %.1fm；本招已进行 %.3f s，Attacking 标签到 %.3f s，速率 %.2f）",
                    Name(r.creature, an), r.melee ? " 近战" : " 非近战", r.deg, r.dist, r.elapsed, r.tagEnd, r.rate);
                // v6.22：这一招的攻击段预读结果，每个招式只记一次
                if (r.move && seenMoves.insert(r.move).second) {
                    MoveAtk a;
                    ReadMoveAtk(r.move, a);
                    Log("    预读 %s 招式 %p（哈希 %016llX，时长 %.3f s）：%d 段攻击%s", an, reinterpret_cast<void*>(r.move),
                        static_cast<unsigned long long>(Rd<uint64_t>(r.move + 0x40)), r.dur, a.n, a.n ? "" : "（招式数据里没有近战段也没有冲击波，可能是飞行物 / 投掷物）");
                    for (int i = 0; i < a.n; ++i) {
                        FmtSeg(sg, sizeof(sg), a.s[i], r.dur);
                        Log("      段 %d：%s", i, sg);
                    }
                }
            } else {
                Log("◆ 出招预警结束 %s（方位 %.0f° 距离 %.1fm）", Name(r.creature, an), r.deg, r.dist);
            }
        }
        for (static uint32_t padTail = 0; padTail != g_padHead.load(); ++padTail) {
            const PadRec& r = g_padRing[padTail & (kPadRing - 1)];
            Log("手柄 e8=%08x 原始10=%08x 有按压量的键=%08x [0..7]=%u %u %u %u %u %u %u %u 摇杆(%.2f,%.2f) 力度%.2f", r.e8, r.raw10,
                r.nz140, r.v140[0], r.v140[1], r.v140[2], r.v140[3], r.v140[4], r.v140[5], r.v140[6], r.v140[7], r.sx, r.sy,
                r.mag);
        }
        if (const uintptr_t ctl = g_heroCtl.load()) { // 被动记录：摇杆推/放、方向变化，用来对照内存布局
            float v[4] = {};
            if (SafeRead(ctl + 0x410, v, sizeof v)) {
                const bool on = v[3] > 0.3f, wasOn = lastMag > 0.3f;
                if (on != wasOn || (on && v[0] * lastDx + v[2] * lastDz < 0.94f)) {
                    float L[12] = {};
                    if (const uintptr_t h = g_hero.load())
                        SafeRead(Rd<uintptr_t>(h + 8) + 0x60, L, sizeof(L));
                    Log("摇杆 方向 (%.2f, %.2f, %.2f) 力度 %.2f ｜ 奎托斯正前方 (%.2f, %.2f)", v[0], v[1], v[2], v[3], -L[8], -L[10]);
                    lastDx = v[0];
                    lastDz = v[2];
                }
                lastMag = v[3];
            }
        }
        if (GetTickCount64() - lastIni > 1000) {
            const bool on = GetPrivateProfileIntW(L"autoparry", L"parry", 1, ini) != 0;
            if (g_parryOn.exchange(on) != on)
                Log("配置：自动弹反 %s", on ? "开" : "关");
            if (GetPrivateProfileIntW(L"autoparry", L"test_evade", 0, ini) != 0) {
                WritePrivateProfileStringW(L"autoparry", L"test_evade", L"0", ini);
                const uintptr_t h = g_hero.load();
                float p0[3] = {};
                if (h && WorldPos(Rd<uintptr_t>(h + 8), p0)) {
                    const uint32_t w0 = g_padWrites.load();
                    float L[12] = {};
                    SafeRead(Rd<uintptr_t>(h + 8) + 0x60, L, sizeof(L));
                    const float bl = sqrtf(L[8] * L[8] + L[10] * L[10]); // 正前方 = -row2，往后闪 = +row2
                    g_evadeDirX.store(bl > 0.f ? L[8] / bl : 0.f);
                    g_evadeDirZ.store(bl > 0.f ? L[10] / bl : 0.f);
                    g_evadeStick.store(GetPrivateProfileIntW(L"autoparry", L"test_stick", 1, ini) != 0);
                    Log("自测：摇杆%s，方向 (%.2f, %.2f)（往奎托斯身后）", g_evadeStick.load() ? "一起推" : "不推",
                        g_evadeDirX.load(), g_evadeDirZ.load());
                    g_evadeRollNow.store(GetPrivateProfileIntW(L"autoparry", L"test_roll", 0, ini) != 0);
                    if (g_evadeRollNow.load())
                        Log("自测：翻滚（按 %lld ms / 松 %lld ms / 再按 %lld ms）", g_rollTap1Us.load() / 1000, g_rollGapUs.load() / 1000,
                            g_rollTap2Us.load() / 1000);
                    g_evadeStart.store(NowUs());
                    Log("自测：替你按一下 ×（%lld ms），奎托斯在 (%.2f, %.2f, %.2f)", g_evadeTapUs.load() / 1000, p0[0], p0[1], p0[2]);
                    auto dumpPad = [](const char* tag) {
                        const uintptr_t pad = g_padLastPad.load();
                        if (!pad)
                            return;
                        for (const uintptr_t off : {uintptr_t{0x10}, uintptr_t{0xe8}}) {
                            const uintptr_t S = pad + off;
                            const uintptr_t ring = Rd<uintptr_t>(S + 0x48);
                            const uint16_t size = Rd<uint16_t>(S + 0x50), head = Rd<uint16_t>(S + 0x52), tail = Rd<uint16_t>(S + 0x54);
                            char line[256];
                            int n = snprintf(line, sizeof line, "%s 状态+%02llx 当前=%04x 环 大小%u 头%u 尾%u 新→旧:", tag,
                                static_cast<unsigned long long>(off), Rd<uint32_t>(S), size, head, tail);
                            for (int i = 0; i < 8 && size && (size & (size - 1)) == 0; ++i)
                                n += snprintf(line + n, sizeof line - n, " %04x",
                                    Rd<uint32_t>(ring + ((tail - 1 - i) & (size - 1)) * 4));
                            Log("%s", line);
                        }
                        Log("%s 选用 %s，允许掩码 %08x", tag, Rd<uint8_t>(pad + 0x375) ? "+10" : "+e8",
                            Rd<uint32_t>(pad + 0x2a8 + Rd<int32_t>(pad + 0x2a4) * 4) & ~Rd<uint32_t>(pad + 0x2c4));
                    };
                    Sleep(30);
                    dumpPad("  按住中");
                    Sleep(90);
                    dumpPad("  刚松开");
                    for (int k = 2; k <= 6; ++k) {
                        Sleep(100);
                        float p[3] = {};
                        WorldPos(Rd<uintptr_t>(h + 8), p);
                        const float dx = p[0] - p0[0], dz = p[2] - p0[2];
                        Log("  +%d ms：移动 %.2f m（写入 × 位 %u 次，摇杆 %u 次）", k * 100, sqrtf(dx * dx + dz * dz),
                            g_padWrites.load() - w0, g_stickWrites.load());
                    }
                    Log("  输入轮询在窗口内跑了 %u 次", g_pollCalls.load());
                    Log("  诊断：判定调用 %u，不是奎托斯 %u（最后一个生物 %p，奎托斯 %p），无手柄 %u，已按着 %u，按键状态地址 %p",
                        g_padCalls.load(), g_padNotHero.load(), reinterpret_cast<void*>(g_padLastCreature.load()),
                        reinterpret_cast<void*>(g_hero.load()), g_padNoPad.load(), g_padAlready.load(),
                        reinterpret_cast<void*>(g_padLastS.load()));
                } else {
                    Log("自测：还没认出奎托斯（进战斗或砍一下东西再试）");
                }
            }
            const bool z = GetPrivateProfileIntW(L"autoparry", L"zone", 0, ini) != 0;
            if (g_anyZone.exchange(z) != z)
                Log("配置：弹反不限命中区域 %s", z ? "开" : "关");
            const bool ev = GetPrivateProfileIntW(L"autoparry", L"evade", 1, ini) != 0;
            if (g_evadeOn.exchange(ev) != ev)
                Log("配置：背后自动闪避 %s", ev ? "开" : "关");
            if (!g_seqRunning)
                g_evadeTapUs.store(static_cast<int64_t>(GetPrivateProfileIntW(L"autoparry", L"evade_ms", 200, ini)) * 1000);
            const bool pr = GetPrivateProfileIntW(L"autoparry", L"predict", 1, ini) != 0;
            if (g_predictOn.exchange(pr) != pr)
                Log("配置：预判闪避 %s", pr ? "开" : "关");
            const int pms = static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"predict_ms", 350, ini));
            if (g_predictLeadMs.exchange(pms) != pms)
                Log("配置：预判提前量 %d ms", pms);
            const int prm = static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"predict_roll_ms", 750, ini));
            if (g_predictRollMs.exchange(prm) != prm)
                Log("配置：翻滚预判提前量 %d ms", prm);
            g_predictDistDm.store(static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"predict_dist", 6, ini)) * 10);
            g_snapOn.store(GetPrivateProfileIntW(L"autoparry", L"snap", 0, ini) != 0);
            g_qteLog.store(GetPrivateProfileIntW(L"autoparry", L"qte_log", 1, ini) != 0);
            const bool qm = GetPrivateProfileIntW(L"autoparry", L"qte_mash", 1, ini) != 0;
            if (g_qteMash.exchange(qm) != qm)
                Log("配置：连按 / 定时单按 QTE 自动完成 %s", qm ? "开" : "关");
            g_projDiag.store(GetPrivateProfileIntW(L"autoparry", L"proj_diag", 0, ini) != 0);
            const bool pp = GetPrivateProfileIntW(L"autoparry", L"proj_pre", 1, ini) != 0;
            if (g_projPre.exchange(pp) != pp)
                Log("配置：飞行物预判弹反 %s", pp ? "开" : "关");
            const int plm = static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"proj_lead_ms", 160, ini));
            if (g_projLeadMs.exchange(plm) != plm)
                Log("配置：飞行物预判提前量 %d ms", plm);
            const bool pev = GetPrivateProfileIntW(L"autoparry", L"proj_evade", 1, ini) != 0;
            if (g_projEvade.exchange(pev) != pev)
                Log("配置：盾接不了 / 背后来的飞行物提前侧闪 %s", pev ? "开" : "关");
            const int pem = static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"proj_evade_ms", 300, ini));
            if (g_projEvadeMs.exchange(pem) != pem)
                Log("配置：飞行物提前侧闪的提前量 %d ms", pem);
            const bool psw = GetPrivateProfileIntW(L"autoparry", L"pred_swallow", 1, ini) != 0;
            if (g_predSwallow.exchange(psw) != psw)
                Log("配置：预判闪避后的碰撞 %s", psw ? "吞掉兜底" : "不吞，交给游戏判定（实验）");
            if (!g_seqRunning) { // 自动测试进行中不覆盖它设的节奏
                g_rollTap1Us.store(static_cast<int64_t>(GetPrivateProfileIntW(L"autoparry", L"roll_tap1", 120, ini)) * 1000);
                g_rollGapUs.store(static_cast<int64_t>(GetPrivateProfileIntW(L"autoparry", L"roll_gap", 280, ini)) * 1000);
                g_rollTap2Us.store(static_cast<int64_t>(GetPrivateProfileIntW(L"autoparry", L"roll_tap2", 150, ini)) * 1000);
            }
            const bool roll = GetPrivateProfileIntW(L"autoparry", L"evade_roll", 0, ini) != 0;
            if (g_evadeRoll.exchange(roll) != roll)
                Log("配置：巨魔类预判闪避 %s", roll ? "翻滚（连按两下 ×）" : "单下闪避");
            const bool side = GetPrivateProfileIntW(L"autoparry", L"evade_side", 1, ini) != 0;
            if (g_evadeSide.exchange(side) != side)
                Log("配置：巨魔类预判闪避方向 %s", side ? "侧闪" : "往后");
            const bool pre = GetPrivateProfileIntW(L"autoparry", L"preread", 1, ini) != 0;
            if (g_preread.exchange(pre) != pre)
                Log("配置：按招式数据预读攻击段、接不住的提前闪 %s", pre ? "开" : "关");
            const int bd = static_cast<int>(GetPrivateProfileIntW(L"autoparry", L"back_deg", 100, ini));
            if (g_backDeg.exchange(bd) != bd)
                Log("配置：背后 = 方位超过 %d°", bd);
            lastIni = GetTickCount64();
            {   // 诊断：奎托斯指针在这 1 s 里是什么、换过几次
                static uintptr_t lastH = 0;
                static uint32_t lastFlips = 0;
                const uint32_t fl = g_heroFlips.load();
                if (g_hero.load() != lastH || fl != lastFlips)
                    Log("奎托斯指针 %p（防御组 %d 套；1 s 内换了 %u 次）", reinterpret_cast<void*>(g_hero.load()),
                        g_hero.load() && Rd<uintptr_t>(g_hero.load() + 0x3a70) ? Rd<int>(Rd<uintptr_t>(g_hero.load() + 0x3a70) + 0x48) : -1,
                        fl - lastFlips);
                lastH = g_hero.load();
                lastFlips = fl;
            }
            // v6.20：游戏自己的玩家指针。读档 / 换场景时会变，读不到（加载中）就清空，钩子里各处见 0 自己会跳过
            if (const uintptr_t hc = NativeHero(); hc != g_hero.load()) {
                Log("奎托斯（游戏的玩家指针）：生物 %p（之前 %p）", reinterpret_cast<void*>(hc), reinterpret_cast<void*>(g_hero.load()));
                g_hero.store(hc);
            }
        }
        const uint32_t head = g_head.load();
        for (; tail != head; ++tail) {
            const Rec& e = g_ring[tail & (kRing - 1)];
            char an[32];
            const double ms = e.sincePress < 0 ? -1.0 : e.sincePress / 1000.0;
            if (e.kind == kBlock) {
                const uintptr_t vol = Rd<uintptr_t>(e.victim + 0x4670);
                Log("  格挡判定 返回=%u 输出=%u 调用者=GoW+%X 匹配防御体=%p(半径 %.1f) 命中标志=%016llX p5=%016llX p6=%016llX 方位 %.0f° %.1fm%s",
                    e.ret, e.out, e.caller, reinterpret_cast<void*>(vol), vol ? Rd<float>(vol + 0x40) : -1.f,
                    static_cast<unsigned long long>(e.flags), static_cast<unsigned long long>(e.p5),
                    static_cast<unsigned long long>(e.p6), e.deg, e.dist,
                    e.ret && e.out ? " ← 弹反" : (e.ret ? " ← 格挡/防住" : " ← 挨打"));
                Log("    %s 出招状态=%#x（0x10/0x20/0x400 才会被弹开） 默认被挡反应=%u 防御体反应=%u", Name(e.attacker, an),
                    e.aiState, e.react, e.matchReact);
            } else if (e.kind == kProjPre) {
                char on[24];
                GoName(e.victim, on);
                Log("◇ 飞行物飞向奎托斯：发射者 %s，还有 %u ms 到，方位 %.0f°，水平距离 %.1fm，偏离胸口 %.2fm，命中标志=%016llX → %s", on,
                    static_cast<uint32_t>(e.p5) / 1000, static_cast<int32_t>(e.p5 >> 32) / 10.f, (e.p6 >> 32) / 100.f,
                    static_cast<uint32_t>(e.p6) / 1000.f, static_cast<unsigned long long>(e.flags),
                    e.cleared ? "自己人射的，不按"
                    : (e.caller & 1) ? (e.ret ? ((e.caller & 2) ? "盾接不了，提前侧闪" : "背后来的，提前侧闪")
                                              : "该闪，但上一次闪避还没做完 / 游戏暂停，留给碰到时兜底")
                    : (e.caller & 2) ? "盾接不了，但没开闪避 / 弹道不够贴身，不动"
                    : (!e.out ? "不在可弹反区域，不按" : (e.ret ? "提前按 L1" : "L1 刚按过/你自己按着，不重按")));
            } else if (e.kind == kShove) {
                Log("  ★ 弹反成功：奎托斯震到 %s（命中标志=%016llX，按下后 %.0f ms）  累计 成功 %u / 尝试 %u",
                    Name(e.victim, an), static_cast<unsigned long long>(e.flags), ms, g_parryOk.load(), g_parryTry.load());
            } else {
                char orig[48] = "";
                if (e.kind == kProjParry)
                    sprintf_s(orig, "（游戏原判定 返回=%u 输出=%u）", e.ret, e.out);
                // v6.21：近战这几类事件的 p5 带完整的 64 位命中标志；飞行物那类的 flags 本身就是完整的
                const bool proj = e.kind == kProjParry;
                const uint64_t hf = proj ? e.flags : e.p5;
                // v6.23：「不可格挡」只认攻击定义的 FUnblockable（近战事件 flags 低 32 位 = +0x44）；高 16 位的 0004 单独标成重击档
                const bool red = !proj && (e.flags & 4) != 0;
                const bool heavyBit = ((hf >> 48) & 4) != 0;
                const bool projHit = proj || (hf & 4) != 0;
                Log("%s 碰到奎托斯：%s%s（方位 %.0f° 距离 %.1fm，按下后 %.0f ms；本招 %.3f s / 标签到 %.3f s；攻击标志 %llX 命中标志 %016llX%s）",
                    Name(e.attacker, an), kKindText[e.kind], orig, e.deg, e.dist, ms, e.mvElapsed, e.mvTagEnd,
                    static_cast<unsigned long long>(e.flags), static_cast<unsigned long long>(hf),
                    red ? " ← 不可格挡（红光），走闪避"
                    : NoShield(hf) ? " ← 盾的掩码接不了这一档，走闪避"
                    : heavyBit ? (projHit ? " ← 飞行物带高位 0004，盾接得住，按可弹反处理" : " ← 重击档（高位 0004），近战照旧走闪避") : "");
                // 每种敌人记一次它定义里的弹反字段（生物 +0x4c30 → 定义；+0x448 ParryReaction / +0x450 ParryMask / +0x458 BlockMask）
                if (const uintptr_t def = e.attacker ? Rd<uintptr_t>(e.attacker + 0x4c30) : 0; def && seenDefs.insert(def).second) {
                    const uintptr_t react = Rd<uintptr_t>(def + 0x448);
                    Log("    敌人定义 %s %p：被弹反时的招式=%p（哈希 %016llX） ParryMask=%016llX BlockMask=%016llX", an,
                        reinterpret_cast<void*>(def), reinterpret_cast<void*>(react),
                        static_cast<unsigned long long>(react ? Rd<uint64_t>(react + 0x40) : 0),
                        static_cast<unsigned long long>(Rd<uint64_t>(def + 0x450)),
                        static_cast<unsigned long long>(Rd<uint64_t>(def + 0x458)));
                }
                // v6.22 自检：真正打中的这一段（碰撞上下文里的攻击定义，p6）在不在这一招预读出来的攻击段里。
                // 不在 = 这类攻击走的是别的数据（飞行物 / 投掷物 / 别的脚本动作），预读还没覆盖到，要补
                if (!proj && e.p6 && e.mvMove && seenHitDefs.insert(e.p6 ^ (static_cast<uint64_t>(e.mvMove) << 17)).second) {
                    MoveAtk a;
                    ReadMoveAtk(e.mvMove, a);
                    int hit = -1;
                    for (int i = 0; i < a.n; ++i)
                        if (a.s[i].def == e.p6)
                            hit = i;
                    char sg[200] = "";
                    if (hit >= 0)
                        FmtSeg(sg, sizeof(sg), a.s[hit], e.mvDur);
                    Log("    预读核对 %s 招式 %p（哈希 %016llX）打中的攻击定义 %p：%s%s（接触时本招 %.3f s）", an, reinterpret_cast<void*>(e.mvMove),
                        static_cast<unsigned long long>(Rd<uint64_t>(e.mvMove + 0x40)), reinterpret_cast<void*>(static_cast<uintptr_t>(e.p6)),
                        hit >= 0 ? "对上了，" : "✗ 不在这一招预读的攻击段里（共预读到几段见上面「预读」行）", sg, e.mvElapsed);
                }
            }
        }
        {
            static uint32_t swTail = 0;
            const uint32_t sh = g_sweepHead.load();
            if (sh - swTail > kSweepRing)
                swTail = sh - kSweepRing;
            for (; swTail != sh; ++swTail) {
                const SweepRec& q = g_sweepRing[swTail & (kSweepRing - 1)];
                Log("  · 扫掠 飞行物=%p 标志=%X from(%.2f,%.2f,%.2f) to(%.2f,%.2f,%.2f) 半径 %.2f ｜ 奎托斯胸口(%.2f,%.2f,%.2f) 还要 %.2f 步 偏离 %.2fm",
                    reinterpret_cast<void*>(q.proj), q.pflags, q.a[0], q.a[1], q.a[2], q.b[0], q.b[1], q.b[2], q.radius,
                    q.hp[0], q.hp[1], q.hp[2], q.t, q.miss);
            }
        }
        const uint32_t dh = g_defHead.load();
        if (dh - defTail > kDefRing)
            defTail = dh - kDefRing;
        for (; defTail != dh; ++defTail) {
            const DefRec& e = g_defRing[defTail & (kDefRing - 1)];
            if (e.what) {
                Log("× %s", e.what == 1 ? "按下" : "松开");
                continue;
            }
            char line[512];
            int len = sprintf_s(line, "防御组 %d 个:", e.n);
            for (int i = 0; i < e.n && len > 0 && len < 450; ++i)
                len += sprintf_s(line + len, sizeof(line) - len, " %p(r%.0f)", reinterpret_cast<void*>(e.vols[i]),
                    Rd<float>(e.vols[i] + 0x40));
            Log("%s", line);
            for (int i = 0; i < e.n; ++i) {
                const uintptr_t v = e.vols[i];
                if (!v || !seenVols.insert(v).second)
                    continue;
                Log("    新防御体 %p：掩码10=%llX 掩码18=%X 哈希20=%llX 条件08=%p 半径40=%.1f", reinterpret_cast<void*>(v),
                    static_cast<unsigned long long>(Rd<uint64_t>(v + 0x10)), Rd<uint32_t>(v + 0x18),
                    static_cast<unsigned long long>(Rd<uint64_t>(v + 0x20)), reinterpret_cast<void*>(Rd<uintptr_t>(v + 8)),
                    Rd<float>(v + 0x40));
                // v6.21 的「盾接不接得住」是按 kShieldMask 判的；万一换了盾 / 升级后掩码变了，这里喊一声好改常量
                if (const uint64_t m10 = Rd<uint64_t>(v + 0x10); m10 != 0x7FFFFFFFFFFFFFFFull && (m10 & kAtkLevelBits & ~kShieldMask))
                    Log("    ！这个防御体的掩码 %llX 有 kShieldMask(%llX) 之外的等级位，NoShield 的常量该更新了",
                        static_cast<unsigned long long>(m10), static_cast<unsigned long long>(kShieldMask));
            }
        }
        {
            const uint32_t qh = g_qHead.load(std::memory_order_acquire);
            if (qh - qTail > kQRing) {
                QLog("（丢了 %u 条，太快）", qh - qTail - kQRing);
                qTail = qh - kQRing;
            }
            for (; qTail != qh; ++qTail) {
                const QRec e = g_qRing[qTail & (kQRing - 1)];
                if (e.owner) {
                    const uintptr_t key = e.prompt ^ 0x8000000000000000ull;
                    auto it2 = qLast.find(key);
                    if (it2 == qLast.end() || it2->second.res != e.res) {
                        qLast[key] = {e.id, e.type, e.res};
                        char on[32];
                        QLog("  【别的上下文】ctx[0]=%p(%s) 提示 %p 动作ID=0x%03X 类型=%u 结果=%u 调用者=GoW+0x%llX 奎托斯招式 %016llX 在招 %.3f s",
                            reinterpret_cast<void*>(e.owner), Name(e.owner, on), reinterpret_cast<void*>(e.prompt), e.id, e.type, e.res,
                            static_cast<unsigned long long>(e.ret - g_exe), static_cast<unsigned long long>(e.moveHash), e.el);
                    }
                    continue;
                }
                if (e.moveHash != qMoveHash) {
                    qMoveHash = e.moveHash;
                    const uintptr_t ms = g_hero.load() ? Rd<uintptr_t>(g_hero.load() + 0x3a70) : 0;
                    QLog("奎托斯招式 → %016llX 时长 %.3f s", static_cast<unsigned long long>(e.moveHash),
                        ms ? Rd<float>(ms + 0xe4) : -1.f);
                }
                auto it = qLast.find(e.prompt);
                if (it == qLast.end()) {
                    qLast[e.prompt] = {e.id, e.type, e.res};
                    const long long off = static_cast<long long>(e.prompt) - static_cast<long long>(e.move);
                    char hex[3 * 0x100 + 1];
                    uint8_t buf[0x100] = {};
                    SafeRead(e.prompt, buf, sizeof(buf));
                    for (int i = 0; i < 0x100; ++i)
                        sprintf_s(hex + i * 3, 4, "%02X%c", buf[i], (i & 15) == 15 ? '|' : ' ');
                    QLog("新提示 %p 动作ID=0x%03X 类型=%u 结果=%u 招式内偏移=%s%llX 在招 %.3f s 原始:%s",
                        reinterpret_cast<void*>(e.prompt), e.id, e.type, e.res, off >= 0 && off < 0x100000 ? "+" : "（不在招式内）",
                        off >= 0 && off < 0x100000 ? off : 0, e.el, hex);
                } else if (it->second.res != e.res || it->second.id != e.id || it->second.type != e.type) {
                    it->second = {e.id, e.type, e.res};
                    QLog("  提示 %p 动作ID=0x%03X 类型=%u → %u（在招 %.3f s）", reinterpret_cast<void*>(e.prompt), e.id, e.type,
                        e.res, e.el);
                }
            }
            {   // 连按 / 定时单按 QTE：开始、结束各记一行（300 ms 没再更新算结束）
                static int64_t mashStart = 0;
                static uint32_t mashTicks0 = 0, timedSeen = 0;
                static const char* const kMashBtn[] = {"△", "○", "×", "□", "R1", "L1", "L2", "R2", "L2+R2", "转摇杆",
                    "顺时针转", "逆时针转", "L1/R1 交替"};
                const int64_t mu = g_mashUs.load(std::memory_order_relaxed);
                const bool active = mu && NowUs() - mu < 300'000;
                if (active && !mashStart) {
                    mashStart = mu;
                    mashTicks0 = g_mashTicks.load();
                    const uint32_t info = g_mashInfo.load();
                    const uint32_t btn = info & 0xFF, flags = info >> 16;
                    QLog("【连按 QTE】开始：按键 %s（类型 %u）记录标志 %04X%s", btn < 13 ? kMashBtn[btn] : "?", btn, flags,
                        btn > 5 ? " ← 扳机 / 摇杆 / 交替类暂不自动，照常手按"
                        : (flags & 0x200) ? " ← 这个记录不许自动完成（游戏自己的规则），要你手按" : "，自动连按中");
                } else if (!active && mashStart) {
                    QLog("【连按 QTE】结束：持续 %.2f s，更新了 %u 帧", (mu - mashStart) / 1e6, g_mashTicks.load() - mashTicks0);
                    mashStart = 0;
                }
                if (const uint32_t tt = g_timedTicks.load(); tt != timedSeen) {
                    QLog("【定时单按 QTE】自动判成功（累计 %u 帧）", tt);
                    timedSeen = tt;
                }
            }
            {   // 圆形按键图标：谁头上亮了 / 灭了哪个图标（给「R3 / L3+R3 提示自动按」找信号用）
                static uint32_t iconTail = 0;
                const uint32_t ih = g_iconHead.load(std::memory_order_acquire);
                if (ih - iconTail > kIconRing)
                    iconTail = ih - kIconRing;
                for (; iconTail != ih; ++iconTail) {
                    const IconRec e = g_iconRing[iconTail & (kIconRing - 1)];
                    char nm[32];
                    QLog("【按键图标】%s %p(%s) 图标号 %d 模式 %u（0 招式期间显示 / 1 关掉 / 2 打开）",
                        e.creature == g_hero.load(std::memory_order_relaxed) ? "奎托斯" : "别的生物",
                        reinterpret_cast<void*>(e.creature), Name(e.creature, nm), e.icon, e.mode);
                }
            }
            if (qLast.size() > 20000)
                qLast.clear();
            if (qf && GetTickCount64() - qFlushAt > 1000) {
                fflush(qf);
                qFlushAt = GetTickCount64();
            }
        }
        const uint32_t f = g_forced.load();
        if (f != lastForced) {
            Log("  L1 条件：被强制成立累计 %u 次（× 累计 %u 次；飞行物扫掠 %u 次）", f, g_evadeForced.load(), g_sweepCalls.load());
            lastForced = f;
        }
    }
    if (ok || g_block.installed || g_cond.installed || g_contact.installed) {
        RemoveVt(g_vtIcon);
        RemoveVt(g_vtTimed);
        RemoveVt(g_vtRepeat);
        Remove(g_sweep);
        Remove(g_poll);
        Remove(g_contact);
        Remove(g_cond);
        Remove(g_block);
        for (int i = 0; i < 200 && g_inflight.load() != 0; ++i)
            Sleep(10);
        Sleep(200);
    }
    if (qf)
        fclose(qf);
    Log("==== 自动弹反实验卸载 ====");
    if (g_log)
        fclose(g_log);
    CloseHandle(unload);
    FreeLibraryAndExitThread(g_self, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
