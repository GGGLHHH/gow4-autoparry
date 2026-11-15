// 战神4 QTE 探针：hook GoW.exe+0x87ac70（提示输入条件判定），只记录、不改游戏行为。
// 每个提示对象的（动作ID, 输入类型, 结果）一变就往 %TEMP%\gow4_probe.log 追加一行。
// 卸载：注入器 SetEvent("Local\gow4_probe_unload") → 原子写回原字节 → 等在途调用清零 → FreeLibraryAndExitThread。
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>

namespace {

constexpr uintptr_t kCheckRva = 0x87ac70;
// mov r11,rsp / push rsi / push r12 / push r13 / sub rsp,0x60
constexpr uint8_t kExpected[12] = {0x4C, 0x8B, 0xDC, 0x56, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x60};
constexpr size_t kStolen = 6; // 搬进跳板的前 6 字节（前三条指令，没有 rip 相对寻址）

using CheckFn = uint64_t(__fastcall*)(void* ctx, uint8_t* prompt, void* out);

struct Record {
    uint64_t tick;
    uintptr_t prompt;
    uintptr_t caller;
    uint16_t id;
    uint8_t type;
    uint8_t result;
    uint16_t f4, f6;
    uint8_t b0c;
};

constexpr uint32_t kRing = 1 << 14;
Record g_ring[kRing];
std::atomic<uint32_t> g_head{0};
std::atomic<int> g_inflight{0};
std::atomic<uint64_t> g_calls{0};

uint8_t* g_target = nullptr;
uint8_t* g_tramp = nullptr;
CheckFn g_orig = nullptr;
uint64_t g_originalQword = 0;
HMODULE g_self = nullptr;
FILE* g_log = nullptr;

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

uint64_t __fastcall Detour(void* ctx, uint8_t* prompt, void* out)
{
    g_inflight.fetch_add(1, std::memory_order_acquire);
    const uint64_t r = g_orig(ctx, prompt, out);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (prompt) {
        Record& e = g_ring[g_head.fetch_add(1, std::memory_order_relaxed) & (kRing - 1)];
        e.tick = GetTickCount64();
        e.prompt = reinterpret_cast<uintptr_t>(prompt);
        e.caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        e.id = *reinterpret_cast<uint16_t*>(prompt + 0xe8);
        e.type = prompt[0xef];
        e.result = static_cast<uint8_t>(r);
        e.f4 = *reinterpret_cast<uint16_t*>(prompt + 0xf4);
        e.f6 = *reinterpret_cast<uint16_t*>(prompt + 0xf6);
        e.b0c = prompt[0xc];
    }
    g_inflight.fetch_sub(1, std::memory_order_release);
    return r;
}

// 在 GoW.exe 附近 ±2GB 内找一块可执行内存，放跳板（rel32 jmp 够得着）
uint8_t* AllocNear(uint8_t* near_)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(near_) & ~(step - 1);
    for (uintptr_t d = step; d < 0x7FF00000; d += step) {
        for (uintptr_t cand : {base - d, base + d}) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(cand), 4096, MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
    }
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
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage < kCheckRva + 16) {
        Log("宿主进程不是 GoW.exe（镜像只有 0x%X 字节），不下 hook", nt->OptionalHeader.SizeOfImage);
        return false;
    }
    g_target = exe + kCheckRva;
    if (memcmp(g_target, kExpected, sizeof(kExpected)) != 0) {
        Log("目标字节不匹配，不下 hook（游戏版本不对或已被别的东西改过）");
        return false;
    }
    g_tramp = AllocNear(g_target);
    if (!g_tramp) {
        Log("找不到 ±2GB 内的跳板内存");
        return false;
    }
    // 跳板: [原前 6 字节][jmp abs → target+6]   中继(+0x40): [jmp abs → Detour]
    memcpy(g_tramp, g_target, kStolen);
    WriteAbsJmp(g_tramp + kStolen, g_target + kStolen);
    uint8_t* relay = g_tramp + 0x40;
    WriteAbsJmp(relay, reinterpret_cast<void*>(&Detour));
    FlushInstructionCache(GetCurrentProcess(), g_tramp, 0x60);
    g_orig = reinterpret_cast<CheckFn>(g_tramp);

    // 目标 16 字节对齐 → 前 8 字节一次原子替换：E9 rel32 + 90 + 原第 7、8 字节
    g_originalQword = *reinterpret_cast<uint64_t*>(g_target);
    uint8_t patch[8];
    memcpy(patch, g_target, 8);
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(relay - (g_target + 5));
    patch[5] = 0x90;
    uint64_t value;
    memcpy(&value, patch, 8);
    if (!SwapQword(g_originalQword, value)) {
        Log("原子写入失败");
        return false;
    }
    Log("hook 已装：GoW.exe=%p 目标=%p 跳板=%p", exe, g_target, g_tramp);
    return true;
}

// 后台：去重后写日志，等卸载信号
DWORD WINAPI Worker(void*)
{
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"gow4_probe.log");
    // 注意：不能用 ccs=UTF-8（那是宽字符流，再用 fprintf 会触发非法参数直接结束游戏进程）。
    // 源码按 /utf-8 编译，窄字符串本身就是 UTF-8，二进制追加即可
    g_log = _wfsopen(path, L"ab", _SH_DENYNO); // 不锁文件：游戏运行中外部也能实时读
    Log("==== 探针加载 ====");

    HANDLE unload = CreateEventW(nullptr, TRUE, FALSE, L"Local\\gow4_probe_unload");
    const bool installed = Install();
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    struct Last {
        uint16_t id;
        uint8_t type, result;
    };
    std::unordered_map<uintptr_t, Last> last;
    uint32_t tail = 0;
    uint64_t lastSummary = GetTickCount64();
    for (;;) {
        const bool stop = !installed || WaitForSingleObject(unload, 50) == WAIT_OBJECT_0;
        const uint32_t head = g_head.load(std::memory_order_acquire);
        if (head - tail > kRing)
            tail = head - kRing;
        for (; tail != head; ++tail) {
            const Record& e = g_ring[tail & (kRing - 1)];
            auto it = last.find(e.prompt);
            if (it != last.end() && it->second.id == e.id && it->second.type == e.type && it->second.result == e.result)
                continue;
            last[e.prompt] = {e.id, e.type, e.result};
            Log("提示=%p 动作ID=0x%03X 类型=%2u 结果=%u f4=%04X f6=%04X b0c=%02X 调用者=GoW+0x%llX", (void*)e.prompt,
                e.id, e.type, e.result, e.f4, e.f6, e.b0c, static_cast<unsigned long long>(e.caller - exe));
        }
        if (GetTickCount64() - lastSummary > 10000) {
            Log("累计调用 %llu 次，跟踪中的提示 %zu 个", static_cast<unsigned long long>(g_calls.load()), last.size());
            lastSummary = GetTickCount64();
            if (last.size() > 4096)
                last.clear();
        }
        if (stop)
            break;
    }

    if (installed) {
        uint64_t current = *reinterpret_cast<uint64_t*>(g_target);
        Log(SwapQword(current, g_originalQword) ? "已恢复原字节" : "恢复原字节失败！");
        for (int i = 0; i < 200 && g_inflight.load() != 0; ++i)
            Sleep(10);
        Sleep(200); // 刚进中继、还没进 Detour 的线程
        // 跳板内存不释放：极少数线程可能还停在里面，留着几 KB 无害
    }
    Log("==== 探针卸载 ====");
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
