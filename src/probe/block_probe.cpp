// 战神4 格挡探针：hook GoW.exe+0x79f0c0（命中结算里的格挡判定），只记录、不改游戏行为。
// 判定：受击者防御组件里有防御体匹配这次命中 → 返回 1（被格挡）；再在防御体半径内 → 输出参数写 1（推测是弹反）。
// 每次调用往 %TEMP%\gow4_blockprobe.log 写一行：攻击者 / 受击者名字、返回值、输出参数、命中标志、受击者 +0x4670 防御体。
// 卸载：gow4_inject unload gow4_blockprobe。
// 定义 GOW4_AUTOPARRY 编出 gow4_autoparry.dll：受击者是奎托斯、游戏判定「被格挡」但没判弹反时，把输出参数改成 1（升级成弹反）。
#ifdef GOW4_AUTOPARRY
#define PROBE_NAME L"gow4_autoparry"
#else
#define PROBE_NAME L"gow4_blockprobe"
#endif
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace {

constexpr uintptr_t kTargetRva = 0x79f0c0;
// mov r11,rsp / mov [r11+10h],rbx / mov [r11+18h],rbp / mov [r11+20h],r9
constexpr uint8_t kExpected[15] = {
    0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49, 0x89, 0x6B, 0x18, 0x4D, 0x89, 0x4B, 0x20};
constexpr size_t kStolen = 7; // 前两条指令，没有 rip 相对寻址

using BlockFn = uint64_t(__fastcall*)(uintptr_t defense, uintptr_t attacker, uintptr_t victim, uint64_t hitFlags,
    uint64_t p5, uint64_t p6, uint8_t* out);

struct Record {
    uint64_t tick;
    uintptr_t caller, defense, attacker, victim;
    uint64_t hitFlags, p5, p6;
    uint8_t ret, out, upgraded;
};

constexpr uint32_t kRing = 1 << 12;
Record g_ring[kRing];
std::atomic<uint32_t> g_head{0};
std::atomic<int> g_inflight{0};

uint8_t* g_target = nullptr;
uint8_t* g_tramp = nullptr;
BlockFn g_orig = nullptr;
uint64_t g_originalQword = 0;
HMODULE g_self = nullptr;
FILE* g_log = nullptr;
uintptr_t g_exe = 0;
std::atomic<uintptr_t> g_hero{0}; // 奎托斯的生物指针（按名字 goheroa00 认出来后缓存）

bool IsHero(uintptr_t creature);

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

uint64_t __fastcall Detour(uintptr_t defense, uintptr_t attacker, uintptr_t victim, uint64_t hitFlags, uint64_t p5,
    uint64_t p6, uint8_t* out)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const uint64_t r = g_orig(defense, attacker, victim, hitFlags, p5, p6, out);
    bool upgraded = false;
#ifdef GOW4_AUTOPARRY
    if (r && out && *out == 0 && IsHero(victim)) {
        *out = 1;
        upgraded = true;
    }
#endif
    Record& e = g_ring[g_head.fetch_add(1, std::memory_order_relaxed) & (kRing - 1)];
    e.tick = GetTickCount64();
    e.caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    e.defense = defense;
    e.attacker = attacker;
    e.victim = victim;
    e.hitFlags = hitFlags;
    e.p5 = p5;
    e.p6 = p6;
    e.ret = static_cast<uint8_t>(r);
    e.out = out ? *out : 0xFF;
    e.upgraded = upgraded;
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
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

// 生物 → 游戏对象（生物+8）→ 名字（对象+0x38）
void CreatureName(uintptr_t creature, char (&buf)[48])
{
    strcpy_s(buf, "-");
    uintptr_t go = 0, str = 0;
    if (!creature || !SafeRead(creature + 8, &go, 8) || !go || !SafeRead(go + 0x38, &str, 8) || !str)
        return;
    char tmp[48] = {};
    if (SafeRead(str, tmp, sizeof(tmp) - 1)) {
        tmp[sizeof(tmp) - 1] = 0;
        strcpy_s(buf, tmp);
    }
}

bool IsHero(uintptr_t creature)
{
    if (!creature)
        return false;
    if (creature == g_hero.load(std::memory_order_relaxed))
        return true;
    char n[48];
    CreatureName(creature, n);
    if (strcmp(n, "goheroa00") != 0)
        return false;
    g_hero.store(creature, std::memory_order_relaxed);
    return true;
}

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

bool SwapQword(uint64_t expect, uint64_t value)
{
    DWORD old;
    if (!VirtualProtect(g_target, 8, PAGE_EXECUTE_READWRITE, &old))
        return false;
    const bool ok = static_cast<uint64_t>(_InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(g_target),
                        static_cast<long long>(value), static_cast<long long>(expect))) == expect;
    VirtualProtect(g_target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_target, 8);
    return ok;
}

bool Install()
{
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    g_exe = reinterpret_cast<uintptr_t>(exe);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage < kTargetRva + 16) {
        Log("宿主进程不是 GoW.exe，不下 hook");
        return false;
    }
    g_target = exe + kTargetRva;
    if (memcmp(g_target, kExpected, sizeof(kExpected)) != 0) {
        Log("目标字节不匹配，不下 hook");
        return false;
    }
    g_tramp = AllocNear(g_target);
    if (!g_tramp) {
        Log("找不到 ±2GB 内的跳板内存");
        return false;
    }
    memcpy(g_tramp, g_target, kStolen);
    WriteAbsJmp(g_tramp + kStolen, g_target + kStolen);
    uint8_t* relay = g_tramp + 0x40;
    WriteAbsJmp(relay, reinterpret_cast<void*>(&Detour));
    FlushInstructionCache(GetCurrentProcess(), g_tramp, 0x60);
    g_orig = reinterpret_cast<BlockFn>(g_tramp);

    // 前 8 字节原子替换：E9 rel32 + 90 90 + 原第 8 字节（第三条指令开头）
    g_originalQword = *reinterpret_cast<uint64_t*>(g_target);
    uint8_t patch[8];
    memcpy(patch, g_target, 8);
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(relay - (g_target + 5));
    patch[5] = 0x90;
    patch[6] = 0x90;
    uint64_t value;
    memcpy(&value, patch, 8);
    if (!SwapQword(g_originalQword, value)) {
        Log("原子写入失败");
        return false;
    }
    Log("hook 已装：目标=%p 跳板=%p", g_target, g_tramp);
    return true;
}

void DumpVolume(uintptr_t vol)
{
    uint8_t b[0x90];
    if (!vol || !SafeRead(vol, b, sizeof(b)))
        return;
    char hex[0x90 * 3 + 1];
    for (size_t i = 0; i < sizeof(b); ++i)
        sprintf_s(hex + i * 3, 4, "%02X ", b[i]);
    float radius;
    int kind;
    memcpy(&radius, b + 0x40, 4);
    memcpy(&kind, b + 0x80, 4);
    Log("    新防御体 %p：+0x40 半径=%.3f  +0x80=%d  前 0x90 字节：%s", reinterpret_cast<void*>(vol), radius, kind, hex);
}

DWORD WINAPI Worker(void*)
{
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, PROBE_NAME L".log");
    g_log = _wfsopen(path, L"ab", _SH_DENYNO);
#ifdef GOW4_AUTOPARRY
    Log("==== 自动弹反（实验：格挡升级成弹反）加载 ====");
#else
    Log("==== 格挡探针加载 ====");
#endif
    HANDLE unload = CreateEventW(nullptr, TRUE, FALSE, L"Local\\" PROBE_NAME L"_unload");
    const bool installed = Install();

    std::unordered_set<uintptr_t> seenVol;
    uint32_t tail = 0;
    for (;;) {
        const bool stop = !installed || WaitForSingleObject(unload, 30) == WAIT_OBJECT_0;
        Sleep(stop ? 0 : 20); // 让调用方把 +0x4670 写完再读
        const uint32_t head = g_head.load(std::memory_order_acquire);
        if (head - tail > kRing)
            tail = head - kRing;
        for (; tail != head; ++tail) {
            const Record& e = g_ring[tail & (kRing - 1)];
            char an[48], vn[48];
            CreatureName(e.attacker, an);
            CreatureName(e.victim, vn);
            uintptr_t vol = 0;
            SafeRead(e.victim + 0x4670, &vol, 8);
            int nVol = -1;
            SafeRead(e.defense + 0x48, &nVol, 4);
            Log("格挡判定 返回=%u 输出=%u%s 攻击者=%s(%p) 受击者=%s(%p) 命中标志=%016llX p5=%llX p6=%llX 防御体数=%d "
                "受击者+4670=%p 调用者=GoW+0x%llX",
                e.ret, e.out, e.upgraded ? "（已升级成弹反）" : "", an, reinterpret_cast<void*>(e.attacker), vn, reinterpret_cast<void*>(e.victim),
                static_cast<unsigned long long>(e.hitFlags), static_cast<unsigned long long>(e.p5),
                static_cast<unsigned long long>(e.p6), nVol, reinterpret_cast<void*>(vol),
                static_cast<unsigned long long>(e.caller - g_exe));
            if (e.ret && vol && seenVol.insert(vol).second)
                DumpVolume(vol);
        }
        if (stop)
            break;
    }
    if (installed) {
        const uint64_t current = *reinterpret_cast<uint64_t*>(g_target);
        Log(SwapQword(current, g_originalQword) ? "已恢复原字节" : "恢复原字节失败！");
        for (int i = 0; i < 200 && g_inflight.load() != 0; ++i)
            Sleep(10);
        Sleep(200);
    }
    Log("==== 格挡探针卸载 ====");
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
