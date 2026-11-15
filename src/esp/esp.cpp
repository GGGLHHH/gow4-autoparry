// 战神4 透视：hook DXGI Present，每帧把附近还没拿的可采集物画到游戏画面上。
// 只读游戏内存，不接管键鼠。配置：DLL 同目录 gow4_esp.ini（每 2 秒重读，改了不用重新注入）。
// 卸载：gow4_inject unload gow4_esp → 渲染线程先关 ImGui → 写回虚表 → 等在途调用清零 → FreeLibraryAndExitThread。
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

namespace {

// ---- 游戏偏移：GoW.exe v1.0.13（sha256 a3b83b87…f4f7），详见 docs/gow4-findings.md ----
// 版本核对点：脚本接口 GetOutputFoV 开头（mov r8,[rcx+10h] / mov rax,[rip→相机管理器] / mov edx,[rax+8F0h]）。
// 别用 0x87ac70：QTE 探针会改那里
constexpr uintptr_t kVersionCheckRva = 0x7a6320;
constexpr uint8_t kVersionBytes[16] = {
    0x4C, 0x8B, 0x41, 0x10, 0x48, 0x8B, 0x05, 0xA5, 0x30, 0xB0, 0x01, 0x8B, 0x90, 0xF0, 0x08, 0x00};
constexpr uintptr_t kObjMgrRva = 0x123b5d8; // 对象管理器
constexpr uintptr_t kCamMgrRva = 0x22a93d0; // 相机管理器（GetPrimaryCamera / GetOutputFoV 用的同一个全局）
constexpr uintptr_t kCamMatrix = 0xaa0;     // 相机世界矩阵：行0 右、行1 上、行2 后（看向 -行2）、行3 位置
constexpr uintptr_t kCamFov = 0x8f0;        // 视角，度
constexpr uint32_t kTakenBit = 0x00200000;  // 物品对象 +0x278 的这一位清零 = 已拿走

enum Kind : uint8_t {
    kChest, kRare, kLegend, kNornir, kSeal, kSilver, kArtifact, kRaven, kLore, kShrine, kMap, kRune, kBag, kBell, kSpinner, kKindCount
};
struct KindInfo {
    const char* label;
    ImU32 color;
};
const KindInfo kKinds[kKindCount] = {
    {"宝箱", IM_COL32(232, 180, 74, 255)},
    {"稀有宝箱", IM_COL32(143, 168, 255, 255)},
    {"传说", IM_COL32(255, 138, 61, 255)},
    {"诺恩宝箱", IM_COL32(179, 136, 255, 255)},
    {"符文封印", IM_COL32(214, 190, 255, 255)},
    {"银币", IM_COL32(201, 209, 217, 255)},
    {"古物", IM_COL32(79, 209, 197, 255)},
    {"奥丁的乌鸦", IM_COL32(46, 229, 157, 255)},
    {"碑文", IM_COL32(224, 201, 166, 255)},
    {"巨人神龛", IM_COL32(79, 195, 247, 255)},
    {"藏宝图", IM_COL32(215, 163, 90, 255)},
    {"符文", IM_COL32(240, 98, 146, 255)},
    {"战利品", IM_COL32(165, 184, 74, 255)},
    {"符文铃铛", IM_COL32(214, 190, 255, 255)},
    {"符文转盘", IM_COL32(214, 190, 255, 255)},
};

struct Item {
    uintptr_t go;
    Kind kind;
    uintptr_t glow; // 古物/符文身边的拾取光效；拿走后光效被隐藏（+0x27c 低字节变 0x13），本体标志位不变
    uintptr_t dep;  // 铃铛所属诺恩宝箱里的那袋战利品；它被拿走（箱子开过了）铃铛就不用再标
};
constexpr int kMaxItems = 512;
struct ItemList {
    Item items[kMaxItems];
    int count;
    uintptr_t player;
};

// ---- 全局状态 ----
HMODULE g_self = nullptr;
uintptr_t g_base = 0;
FILE* g_log = nullptr;
wchar_t g_iniPath[MAX_PATH];

SRWLOCK g_listLock = SRWLOCK_INIT;
ItemList g_shared{}; // 扫描线程写，渲染线程拷走

struct Config {
    float radius = 60.f;
    bool fovHorizontal = true; // 73.73° 是水平还是垂直视角，待校准
    float scale = 1.f;
};
Config g_cfg; // 简单值，撕裂无害

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
constexpr int kPresentIdx = 8, kPresent1Idx = 22;
void** g_vtbl = nullptr;
PresentFn g_origPresent = nullptr;
Present1Fn g_origPresent1 = nullptr;

std::atomic<int> g_inflight{0};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_renderDown{false};
std::atomic<uint64_t> g_frames{0};
std::atomic<int> g_drawn{0};
thread_local bool t_inHook = false;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ImFont* g_font = nullptr;
bool g_renderFailed = false;
bool g_selfTest = false; // 环境变量 GOW4_ESP_SELFTEST：在测试宿主里跑，不读游戏
LARGE_INTEGER g_qpf, g_lastQpc;

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

// ---- 读游戏内存（SEH 保护：对象随时可能被游戏释放）----
bool SafeRead(const void* src, void* dst, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 世界坐标 = 本地平移 × 父对象世界矩阵（GO+0xf0 是游戏缓存的父矩阵，+0x148 是父链接类型）
bool ReadWorld(uintptr_t go, float out[3])
{
    __try {
        const float* L = reinterpret_cast<const float*>(go + 0x60);
        const float* P = reinterpret_cast<const float*>(go + 0xf0);
        const int kind = *reinterpret_cast<const int*>(go + 0x148);
        const float lx = L[12], ly = L[13], lz = L[14];
        bool hasParent = false;
        if (kind >= 2 && kind <= 4)
            for (int i = 0; i < 16 && !hasParent; ++i)
                hasParent = P[i] != 0.f;
        for (int c = 0; c < 3; ++c)
            out[c] = hasParent ? lx * P[c] + ly * P[4 + c] + lz * P[8 + c] + P[12 + c] : L[12 + c];
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool StartsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

bool EndsWith(const char* s, const char* p)
{
    const size_t a = strlen(s), b = strlen(p);
    return a >= b && strcmp(s + a - b, p) == 0;
}

// 只认真正被拿走的那个物品对象，不认容器 / 脚本 / 特效。规则和 tools/collectibles.py 一致
int Classify(const char* n)
{
    if (EndsWith(n, "_smsh"))
        return -1;
    if (StartsWith(n, "gochestpouchlegendary") || StartsWith(n, "gopouchlegendary"))
        return kLegend;
    if (StartsWith(n, "gochestpouchrare"))
        return kRare;
    if (StartsWith(n, "gochestpouch"))
        return kChest;
    if (strcmp(n, "gopouchsilver") == 0)
        return kSilver;
    if (StartsWith(n, "goartifact") && !strstr(n, "script") && !strstr(n, "fx") && !strstr(n, "group") &&
        !strstr(n, "mesh") && !strstr(n, "root"))
        return kArtifact;
    if (StartsWith(n, "goprecisionchallenge_raven") || StartsWith(n, "goprecision_challenge_raven"))
        return kRaven;
    if (StartsWith(n, "gorunic") && EndsWith(n, "_break")) // 诺恩宝箱的三个符文封印 gorunic1/2/3_break
        return kSeal;
    // 诺恩宝箱的另一种机关：限时敲响三个符文铃铛。每个铃铛（关卡里叫 goriv420bell_01 之类，名字带关卡号不通用）下面有
    // gobellgeo（架子根部）、gobelltargetfront / gobelltargetback（铃铛本体，同一位置）、gorunesymbol1..3；只认 front 那一个
    if (strcmp(n, "gobelltargetfront") == 0)
        return kBell;
    // 第三种机关：转符文转盘（gospinner_02 / _03 下面的 gospinner，叠在一根柱子上）。名字太普通，
    // 所以 Scan 里只留「按层级确实挂在某个诺恩宝箱那一组下面」的，别的丢掉
    if (strcmp(n, "gospinner") == 0)
        return kSpinner;
    if (StartsWith(n, "goquestloremarker") || StartsWith(n, "gopquestloremarker"))
        return kLore;
    if (strcmp(n, "gotriptych") == 0)
        return kShrine;
    if (StartsWith(n, "gotreasuremapinteract"))
        return kMap;
    if (strstr(n, "healthrune"))
        return kRune;
    if (strcmp(n, "golootbag") == 0)
        return kBag;
    return -1;
}

bool ReadName(uintptr_t go, char (&out)[64])
{
    __try {
        const char* p = *reinterpret_cast<const char* const*>(go + 0x38);
        if (!p)
            return false;
        int i = 0;
        for (; i < 63 && p[i]; ++i)
            out[i] = p[i];
        out[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ReadPtr(uintptr_t a)
{
    uintptr_t v = 0;
    SafeRead(reinterpret_cast<void*>(a), &v, 8);
    return v;
}

// 父对象：GO+0x28（+0x140 是同一个指针）。只认名字读得出来、以 go 开头的
uintptr_t ParentOf(uintptr_t go)
{
    const uintptr_t p = ReadPtr(go + 0x28);
    char nm[64];
    return (p && p != go && ReadName(p, nm) && nm[0] == 'g' && nm[1] == 'o') ? p : 0;
}

// 这袋战利品是不是诺恩宝箱里的：父链上有 gochest_locked_parent（或 gochest_runic_*）。是的话 group = 箱子根对象
// （父链上最上面那个 gochest_* ）的父对象，机关和箱子都挂在它下面。实读（2026-11-15）两种布局：
//   铃铛型  gochestpouchlegendary ← golootobject ← gochest_locked_parent ← gochest_runic_tier1_riv420_1 ← goriv420_cbt（区域组）
//   转盘型  gochestpouchlegendary ← golootobject ← gochest_locked_parent ← gochest_locked_tier3_riv475_1 ← go____spinchest_setup____ ← goriv475_cbt
// 普通传说宝箱是 … ← gochest_legendary_parent ← gochest_legendary_scripted ← 区域组，不算
bool NornirGroup(uintptr_t go, uintptr_t& group)
{
    uintptr_t root = 0;
    bool nornir = false;
    char nm[64];
    for (int d = 0; go && d < 8; ++d, go = ParentOf(go)) {
        if (!ReadName(go, nm) || !StartsWith(nm, "gochest_"))
            continue;
        root = go;
        nornir |= strcmp(nm, "gochest_locked_parent") == 0 || StartsWith(nm, "gochest_runic");
    }
    if (!nornir)
        return false;
    group = ParentOf(root);
    return true;
}

// go 是不是挂在 group 下面（往上最多 10 层）。
// 铃铛实读：gobelltargetfront ← goanimgroup ← gobellgeo ← gobelltarget_small_root ← goriv420bell_01 ← goriv420_cbt（和箱子同一个区域组）
bool IsUnder(uintptr_t go, uintptr_t group)
{
    for (int d = 0; go && d < 10; ++d, go = ParentOf(go))
        if (go == group)
            return true;
    return false;
}

// 遍历全部对象（约 3.4 万个），挑出可采集物和玩家
void Scan(ItemList& L)
{
    L.count = 0;
    L.player = 0;
    float nornir[32][3];
    int nNornir = 0;
    static uintptr_t glows[256];
    static float glowPos[256][3];
    int nGlow = 0;
    const uintptr_t M = ReadPtr(g_base + kObjMgrRva);
    if (!M)
        return;
    char name[64];
    for (const uintptr_t bank : {M + 0x10, M + 0x1918}) {
        uint32_t n = 0;
        SafeRead(reinterpret_cast<void*>(bank + 0x1900), &n, 4);
        n = std::min<uint32_t>(n, 800);
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t E = ReadPtr(bank + i * 8);
            if (!E)
                continue;
            uintptr_t node = ReadPtr(E + 0x50);
            for (int guard = 0; node && guard < 100000; ++guard, node = ReadPtr(node)) {
                const uintptr_t go = node - 0x10;
                if (!ReadName(go, name) || name[0] != 'g' || name[1] != 'o')
                    continue;
                if (!L.player && strcmp(name, "goheroa00") == 0) {
                    float w[3];
                    if (ReadWorld(go, w) && (w[0] != 0.f || w[1] != 0.f || w[2] != 0.f))
                        L.player = go;
                    continue;
                }
                // 诺恩宝箱的位置：gochest_runic_*（开过的箱子可能不再有这个）或 gochest_locked_parent（开没开过都在）
                if ((StartsWith(name, "gochest_runic") || strcmp(name, "gochest_locked_parent") == 0) && nNornir < 32 &&
                    ReadWorld(go, nornir[nNornir]))
                    ++nNornir;
                if (StartsWith(name, "gopickup_glow") && nGlow < 256 && ReadWorld(go, glowPos[nGlow]))
                    glows[nGlow++] = go;
                const int k = Classify(name);
                if (k >= 0 && L.count < kMaxItems)
                    L.items[L.count++] = {go, static_cast<Kind>(k), 0, 0};
            }
        }
    }
    // 同一处同类别常叠 2~4 个对象（比如 goartifacttoy04 和 goartifacttoy04_new），只留一个
    int kept = 0;
    float pos[kMaxItems][3];
    static uintptr_t group[kMaxItems]; // 诺恩宝箱战利品所在的区域组（箱子根对象的父对象），别的类别是 0
    for (int i = 0; i < L.count; ++i) {
        float w[3];
        if (!ReadWorld(L.items[i].go, w))
            continue;
        bool dup = false;
        for (int j = 0; j < kept && !dup; ++j) {
            const float dx = w[0] - pos[j][0], dy = w[1] - pos[j][1], dz = w[2] - pos[j][2];
            // 转盘是几节叠在一根柱子上（实读相隔 1.1 m），一根柱子只标一个
            dup = L.items[j].kind == L.items[i].kind &&
                  dx * dx + dy * dy + dz * dz < (L.items[i].kind == kSpinner ? 2.5f * 2.5f : 0.25f);
        }
        if (dup)
            continue;
        // 传说战利品是不是诺恩宝箱里的：先按游戏自己的父子层级认（它挂在箱子下面），认不出再按距离（箱子 2 m 内）兜底
        uintptr_t grp = 0;
        if (L.items[i].kind == kLegend && NornirGroup(L.items[i].go, grp))
            L.items[i].kind = kNornir;
        for (int j = 0; j < nNornir && L.items[i].kind == kLegend; ++j) {
            const float dx = w[0] - nornir[j][0], dy = w[1] - nornir[j][1], dz = w[2] - nornir[j][2];
            if (dx * dx + dy * dy + dz * dz < 4.f)
                L.items[i].kind = kNornir;
        }
        if (L.items[i].kind == kArtifact || L.items[i].kind == kRune) {
            float best = 1.5f * 1.5f;
            for (int j = 0; j < nGlow; ++j) {
                const float dx = w[0] - glowPos[j][0], dy = w[1] - glowPos[j][1], dz = w[2] - glowPos[j][2];
                if (dx * dx + dy * dy + dz * dz < best) {
                    best = dx * dx + dy * dy + dz * dz;
                    L.items[i].glow = glows[j];
                }
            }
        }
        memcpy(pos[kept], w, sizeof(w));
        group[kept] = grp;
        L.items[kept++] = L.items[i];
    }
    L.count = kept;
    // 铃铛 / 符文封印 / 转盘是诺恩宝箱的机关：挂到它所属箱子的战利品上，箱子开过（战利品已拿）就不再标。
    // 所属关系先按游戏自己的层级：机关和箱子挂在同一个组下面（同组有多个箱子取最近的）。
    // 层级对不上时，铃铛再按距离兜底（120 m 内最近的诺恩宝箱）；封印不兜底，照旧只看它自己的状态位；转盘直接丢掉（见 Classify）
    static uintptr_t logged[32][2];
    static uint32_t nLogged = 0;
    for (int i = 0; i < kept; ++i) {
        if (L.items[i].kind != kBell && L.items[i].kind != kSeal && L.items[i].kind != kSpinner)
            continue;
        float best = FLT_MAX;
        bool byTree = false;
        for (int j = 0; j < kept; ++j) {
            if (L.items[j].kind != kNornir || !group[j] || !IsUnder(L.items[i].go, group[j]))
                continue;
            const float dx = pos[i][0] - pos[j][0], dy = pos[i][1] - pos[j][1], dz = pos[i][2] - pos[j][2];
            if (dx * dx + dy * dy + dz * dz < best) {
                best = dx * dx + dy * dy + dz * dz;
                L.items[i].dep = L.items[j].go;
                byTree = true;
            }
        }
        if (!byTree && L.items[i].kind == kBell) {
            best = 120.f * 120.f;
            for (int j = 0; j < kept; ++j) {
                if (L.items[j].kind != kNornir)
                    continue;
                const float dx = pos[i][0] - pos[j][0], dy = pos[i][1] - pos[j][1], dz = pos[i][2] - pos[j][2];
                if (dx * dx + dy * dy + dz * dz < best) {
                    best = dx * dx + dy * dy + dz * dz;
                    L.items[i].dep = L.items[j].go;
                }
            }
        }
        if (!byTree && L.items[i].kind == kSpinner) {
            L.items[i].go = 0; // 不属于任何诺恩宝箱的 gospinner：下面压缩时丢掉
            continue;
        }
        bool seen = false;
        for (const auto& l : logged)
            seen |= l[0] == L.items[i].go && l[1] == L.items[i].dep;
        if (!seen) { // 每个「机关 → 箱子」的对应只记一次
            logged[nLogged % 32][0] = L.items[i].go;
            logged[nLogged++ % 32][1] = L.items[i].dep;
            if (L.items[i].dep)
                Log("%s %p 属于诺恩宝箱战利品 %p（%s，相距 %.0f m）：箱子开过就不再标", kKinds[L.items[i].kind].label,
                    reinterpret_cast<void*>(L.items[i].go), reinterpret_cast<void*>(L.items[i].dep),
                    byTree ? "按层级：同一个区域组" : "层级对不上，按距离", std::sqrt(best));
            else
                Log("%s %p 没找到所属的诺恩宝箱，照常标", kKinds[L.items[i].kind].label, reinterpret_cast<void*>(L.items[i].go));
        }
    }
    int out = 0;
    for (int i = 0; i < kept; ++i)
        if (L.items[i].go)
            L.items[out++] = L.items[i];
    L.count = out;
}

// ---- 绘制 ----
struct Tag {
    float x, y, dist, dy;
    Kind kind;
};

void DrawTag(ImDrawList* dl, const Tag& t, float s, float radius)
{
    const KindInfo& k = kKinds[t.kind];
    const float near_ = 10.f;
    const float fade = t.dist <= near_ ? 1.f : 1.f - 0.5f * std::min(1.f, (t.dist - near_) / std::max(1.f, radius - near_));
    const auto A = [&](ImU32 c, float a) { return (c & 0x00FFFFFF) | (static_cast<ImU32>(((c >> 24) & 0xFF) * a * fade) << 24); };

    // 物品所在点：菱形
    const float r = 4.5f * s;
    const ImVec2 c(t.x, t.y);
    dl->AddQuadFilled({c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}, A(k.color, 1.f));
    dl->AddQuad({c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}, A(IM_COL32(10, 10, 12, 255), 0.9f), 1.2f * s);

    // 标签：类别（彩色）+ 距离（灰）+ 高低箭头
    char dist[32];
    const char* arrow = t.dy > 2.5f ? " ↑" : (t.dy < -2.5f ? " ↓" : "");
    snprintf(dist, sizeof(dist), "%.0fm%s", t.dist, arrow);
    const float fs = 20.f * s;
    const ImVec2 a = g_font->CalcTextSizeA(fs, FLT_MAX, 0.f, k.label);
    const ImVec2 b = g_font->CalcTextSizeA(fs * 0.88f, FLT_MAX, 0.f, dist);
    const float padX = 7.f * s, padY = 3.f * s, gap = 6.f * s, bar = 3.f * s;
    const float w = bar + padX + a.x + gap + b.x + padX, h = a.y + padY * 2;
    const ImVec2 p0(c.x - w * 0.5f, c.y - r - 6.f * s - h), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, A(IM_COL32(14, 16, 20, 200), 1.f), 4.f * s);
    dl->AddRectFilled(p0, {p0.x + bar, p1.y}, A(k.color, 1.f), 4.f * s, ImDrawFlags_RoundCornersLeft);
    dl->AddLine({c.x, p1.y}, {c.x, c.y - r}, A(k.color, 0.6f), 1.f * s);
    const float tx = p0.x + bar + padX;
    dl->AddText(g_font, fs, {tx + 1, p0.y + padY + 1}, A(IM_COL32(0, 0, 0, 255), 0.6f), k.label);
    dl->AddText(g_font, fs, {tx, p0.y + padY}, A(k.color, 1.f), k.label);
    dl->AddText(g_font, fs * 0.88f, {tx + a.x + gap, p0.y + padY + (a.y - b.y) * 0.5f}, A(IM_COL32(200, 204, 210, 255), 1.f),
        dist);
}

ItemList t_local; // 只在渲染线程用

void DrawItems(ImVec2 ds)
{
    AcquireSRWLockShared(&g_listLock);
    t_local.count = g_shared.count;
    t_local.player = g_shared.player;
    memcpy(t_local.items, g_shared.items, sizeof(Item) * g_shared.count);
    ReleaseSRWLockShared(&g_listLock);

    if (g_selfTest) {
        const float s = ds.y / 1080.f * g_cfg.scale;
        for (int k = 0; k < kKindCount; ++k)
            DrawTag(ImGui::GetBackgroundDrawList(),
                {ds.x * 0.5f + (k - kKindCount / 2) * 150.f * s, ds.y * 0.5f + (k % 2) * 60.f * s, 5.f + k * 9.f,
                    k == 2 ? 6.f : (k == 3 ? -6.f : 0.f), static_cast<Kind>(k)},
                s, g_cfg.radius);
        g_drawn.store(kKindCount, std::memory_order_relaxed);
        return;
    }
    const uintptr_t camMgr = ReadPtr(g_base + kCamMgrRva);
    float cam[16], fov = 0, me[3];
    if (!camMgr || !t_local.player || !SafeRead(reinterpret_cast<void*>(camMgr + kCamMatrix), cam, sizeof(cam)) ||
        !SafeRead(reinterpret_cast<void*>(camMgr + kCamFov), &fov, 4) || !ReadWorld(t_local.player, me))
        return;
    if (!(fov > 10.f && fov < 170.f))
        return;
    const Config cfg = g_cfg;
    const float half = std::tan(fov * 0.5f * 3.14159265f / 180.f);
    const float f = (cfg.fovHorizontal ? ds.x : ds.y) * 0.5f / half;

    static Tag tags[kMaxItems];
    int n = 0;
    for (int i = 0; i < t_local.count; ++i) {
        const Item& it = t_local.items[i];
        uint32_t flags = 0;
        float w[3];
        if (!SafeRead(reinterpret_cast<void*>(it.go + 0x278), &flags, 4) || !(flags & kTakenBit) || !ReadWorld(it.go, w))
            continue;
        uint32_t glowHide = 0;
        if (it.glow && SafeRead(reinterpret_cast<void*>(it.glow + 0x27c), &glowHide, 4) && (glowHide & 0xFF))
            continue; // 光效被隐藏 = 古物/符文已拿走
        uint32_t depFlags = 0;
        if (it.dep && SafeRead(reinterpret_cast<void*>(it.dep + 0x278), &depFlags, 4) && !(depFlags & kTakenBit))
            continue; // 所属的诺恩宝箱已经开过，铃铛不用再标
        const float ex = w[0] - me[0], ey = w[1] - me[1], ez = w[2] - me[2];
        const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
        if (dist > cfg.radius)
            continue;
        const float d[3] = {w[0] - cam[12], w[1] - cam[13], w[2] - cam[14]};
        const float x = d[0] * cam[0] + d[1] * cam[1] + d[2] * cam[2];
        const float y = d[0] * cam[4] + d[1] * cam[5] + d[2] * cam[6];
        const float z = -(d[0] * cam[8] + d[1] * cam[9] + d[2] * cam[10]);
        if (z < 0.3f)
            continue;
        const float sx = ds.x * 0.5f + x * f / z, sy = ds.y * 0.5f - y * f / z;
        if (sx < -50 || sx > ds.x + 50 || sy < -50 || sy > ds.y + 50)
            continue;
        tags[n++] = {sx, sy, dist, ey, it.kind};
    }
    std::sort(tags, tags + n, [](const Tag& a, const Tag& b) { return a.dist > b.dist; }); // 远的先画，近的压在上面
    const float s = ds.y / 1080.f * cfg.scale;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < n; ++i)
        DrawTag(dl, tags[i], s, cfg.radius);
    g_drawn.store(n, std::memory_order_relaxed);
}

void ShutdownRender()
{
    if (g_device) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_ctx->Release();
        g_device->Release();
        g_ctx = nullptr;
        g_device = nullptr;
    }
    g_renderDown = true;
}

bool InitRender(IDXGISwapChain* sc)
{
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) {
        Log("交换链不是 D3D11 设备，透视不画");
        return false;
    }
    g_device->GetImmediateContext(&g_ctx);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    g_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 17.f);
    if (!g_font) {
        Log("没找到微软雅黑，用 ImGui 默认字体（中文会显示成方块）");
        g_font = io.Fonts->AddFontDefault();
    }
    if (!ImGui_ImplDX11_Init(g_device, g_ctx)) {
        Log("ImGui DX11 初始化失败");
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_lastQpc);
    Log("渲染初始化完成 device=%p", g_device);
    return true;
}

ID3D11RenderTargetView* MakeRtv(IDXGISwapChain* sc, UINT& w, UINT& h)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))))
        return nullptr;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    w = d.Width;
    h = d.Height;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_device->CreateRenderTargetView(bb, nullptr, &rtv))) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        switch (d.Format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        default: rd.Format = d.Format;
        }
        if (FAILED(g_device->CreateRenderTargetView(bb, &rd, &rtv)))
            rtv = nullptr;
    }
    bb->Release();
    return rtv;
}

void Frame(IDXGISwapChain* sc)
{
    if (g_unloading) {
        if (!g_renderDown)
            ShutdownRender();
        return;
    }
    if (g_renderFailed || g_renderDown)
        return;
    if (!g_device && !InitRender(sc)) {
        g_renderFailed = true;
        return;
    }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))))
        return;
    dev->Release();
    if (dev != g_device) // 别的设备的交换链（比如游戏重建了设备），不画
        return;

    UINT w = 0, h = 0;
    ID3D11RenderTargetView* rtv = MakeRtv(sc, w, h);
    if (!rtv) {
        static bool once = false;
        if (!once)
            Log("创建渲染目标失败");
        once = true;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = std::max(1e-4f, static_cast<float>(now.QuadPart - g_lastQpc.QuadPart) / g_qpf.QuadPart);
    g_lastQpc = now;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    DrawItems(io.DisplaySize);
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (auto* v : oldRtv)
        if (v)
            v->Release();
    if (oldDsv)
        oldDsv->Release();
    rtv->Release();
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const bool outer = !t_inHook; // 原函数里若再走虚表的另一个 Present，不重复画
    t_inHook = true;
    if (outer && !(flags & DXGI_PRESENT_TEST))
        Frame(sc);
    const HRESULT r = g_origPresent(sc, sync, flags);
    t_inHook = !outer;
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const bool outer = !t_inHook; // 原函数里若再走虚表的另一个 Present，不重复画
    t_inHook = true;
    if (outer && !(flags & DXGI_PRESENT_TEST))
        Frame(sc);
    const HRESULT r = g_origPresent1(sc, sync, flags, p);
    t_inHook = !outer;
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

// ---- 安装 / 卸载 hook：改 dxgi.dll 里交换链虚表的 Present / Present1 两项 ----
bool SwapVtbl(int idx, void* value, void** old)
{
    DWORD prot;
    if (!VirtualProtect(&g_vtbl[idx], sizeof(void*), PAGE_READWRITE, &prot))
        return false;
    void* prev = InterlockedExchangePointer(&g_vtbl[idx], value);
    VirtualProtect(&g_vtbl[idx], sizeof(void*), prot, &prot);
    if (old)
        *old = prev;
    return true;
}

bool InstallHook()
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_self;
    wc.lpszClassName = L"gow4_esp_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc = {64, 64, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        g_vtbl = *reinterpret_cast<void***>(sc);
        void* p0 = nullptr;
        void* p1 = nullptr;
        ok = SwapVtbl(kPresentIdx, reinterpret_cast<void*>(&HookPresent), &p0);
        g_origPresent = reinterpret_cast<PresentFn>(p0);
        if (ok && SwapVtbl(kPresent1Idx, reinterpret_cast<void*>(&HookPresent1), &p1))
            g_origPresent1 = reinterpret_cast<Present1Fn>(p1);
        Log("虚表=%p Present=%p Present1=%p", g_vtbl, p0, p1);
        ctx->Release();
        dev->Release();
        sc->Release();
    } else {
        Log("创建临时交换链失败 hr=0x%08lX", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, g_self);
    return ok;
}

void RemoveHook()
{
    if (g_origPresent1)
        SwapVtbl(kPresent1Idx, reinterpret_cast<void*>(g_origPresent1), nullptr);
    if (g_origPresent)
        SwapVtbl(kPresentIdx, reinterpret_cast<void*>(g_origPresent), nullptr);
}

void LoadConfig()
{
    wchar_t buf[32];
    GetPrivateProfileStringW(L"esp", L"radius", L"60", buf, 32, g_iniPath);
    g_cfg.radius = std::clamp(static_cast<float>(_wtof(buf)), 5.f, 500.f);
    GetPrivateProfileStringW(L"esp", L"fov", L"h", buf, 32, g_iniPath);
    g_cfg.fovHorizontal = buf[0] != L'v' && buf[0] != L'V';
    GetPrivateProfileStringW(L"esp", L"scale", L"1", buf, 32, g_iniPath);
    g_cfg.scale = std::clamp(static_cast<float>(_wtof(buf)), 0.5f, 3.f);
}

DWORD WINAPI Worker(void*)
{
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"gow4_esp.log");
    g_log = _wfsopen(path, L"ab", _SH_DENYNO);
    Log("==== 透视加载 ====");
    GetModuleFileNameW(g_self, g_iniPath, MAX_PATH);
    wcscpy_s(wcsrchr(g_iniPath, L'\\') + 1, MAX_PATH - (wcsrchr(g_iniPath, L'\\') + 1 - g_iniPath), L"gow4_esp.ini");
    LoadConfig();

    HANDLE unload = CreateEventW(nullptr, TRUE, FALSE, L"Local\\gow4_esp_unload");
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    bool installed = false;
    g_selfTest = GetEnvironmentVariableW(L"GOW4_ESP_SELFTEST", nullptr, 0) > 0;
    if (g_selfTest)
        installed = InstallHook();
    else if (nt->OptionalHeader.SizeOfImage < kObjMgrRva + 8 ||
        memcmp(reinterpret_cast<void*>(g_base + kVersionCheckRva), kVersionBytes, sizeof(kVersionBytes)) != 0)
        Log("不是 v1.0.13 的 GoW.exe，不装");
    else
        installed = InstallHook();

    static ItemList scratch;
    uint64_t lastScan = 0, lastCfg = 0, lastStat = GetTickCount64();
    while (installed && WaitForSingleObject(unload, 50) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        if (!g_selfTest && now - lastScan >= 500) {
            Scan(scratch);
            AcquireSRWLockExclusive(&g_listLock);
            g_shared.count = scratch.count;
            g_shared.player = scratch.player;
            memcpy(g_shared.items, scratch.items, sizeof(Item) * scratch.count);
            ReleaseSRWLockExclusive(&g_listLock);
            lastScan = now;
        }
        if (now - lastCfg >= 2000) {
            LoadConfig();
            lastCfg = now;
        }
        if (now - lastStat >= 10000) {
            Log("帧 %llu，候选 %d 个，本帧画了 %d 个，玩家 %p，半径 %.0f，视角按%s", g_frames.load(), scratch.count,
                g_drawn.load(), reinterpret_cast<void*>(scratch.player), g_cfg.radius, g_cfg.fovHorizontal ? "水平" : "垂直");
            lastStat = now;
        }
    }

    if (installed) {
        g_unloading = true;
        for (int i = 0; i < 150 && !g_renderDown; ++i) // 让渲染线程自己关 ImGui（最多等 1.5 秒）
            Sleep(10);
        RemoveHook();
        for (int i = 0; i < 200 && g_inflight.load() != 0; ++i)
            Sleep(10);
        Sleep(200); // 刚从虚表跳进 hook、还没加计数的线程
        if (!g_renderDown) // 游戏没在出帧（最小化 / 暂停），在这里关
            ShutdownRender();
        Log("已恢复虚表");
    }
    Log("==== 透视卸载 ====");
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
