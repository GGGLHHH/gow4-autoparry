// gow4_trainer 的配置读写测试（控制台程序，不开窗口、不碰游戏、不碰 build\Release 里正在用的 ini）。
// 全部在 %TEMP%\gow4_trainer_tests_<进程号>\ 里做，跑完删掉。退出码 0 = 全过。
// 用法：trainer_tests.exe

#include <windows.h>

#include <cstdio>
#include <string>

#include "settings.h"

namespace {

int g_failed = 0;
int g_checks = 0;
const char* g_test = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("  FAIL %s (%s:%d): %s\n", g_test, __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

std::wstring g_dir; // 末尾带反斜杠

std::wstring ParryIni() { return g_dir + gow4::kParryIni; }
std::wstring EspIni() { return g_dir + gow4::kEspIni; }
std::wstring AppIni() { return g_dir + L"trainer.ini"; }

void WriteFileBytes(const std::wstring& path, const std::string& bytes)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
}

std::string ReadFileBytes(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    std::string out(GetFileSize(f, nullptr), '\0');
    DWORD read = 0;
    ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(f);
    return out;
}

// 把文件的修改时间往后拨 seconds 秒（外部改动测试用：同一秒内连写两次，修改时间可能一样）
void BumpWriteTime(const std::wstring& path, int seconds)
{
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t{ { now.dwLowDateTime, now.dwHighDateTime } };
    t.QuadPart += static_cast<ULONGLONG>(seconds) * 10'000'000ull;
    const FILETIME bumped{ t.LowPart, t.HighPart };
    SetFileTime(f, nullptr, nullptr, &bumped);
    CloseHandle(f);
}

void CleanDir()
{
    DeleteFileW(ParryIni().c_str());
    DeleteFileW(EspIni().c_str());
    DeleteFileW(AppIni().c_str());
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// 测试用的事件名：不能用真名，否则游戏里的 DLL 开着时会被这里的写入叫醒
std::wstring g_parryEvent, g_espEvent;
StoreEvents TestEvents()
{
    return StoreEvents{ g_parryEvent.c_str(), g_espEvent.c_str() };
}

SettingsStore MakeStore()
{
    return SettingsStore(g_dir, AppIni(), TestEvents());
}

// 带中文注释的弹反 ini（UTF-8），和仓库里的写法一样
const char kParryFile[] =
    "; 自动弹反 / 自动闪避配置，DLL 每秒重读，改完不用重启\r\n"
    "[autoparry]\r\n"
    "; 自动弹反 1 开 / 0 关\r\n"
    "parry=1\r\n"
    "; 离出伤还剩多少毫秒时按 ×（越大闪得越早）\r\n"
    "predict_ms=350\r\n"
    "evade_ms=200\r\n";

void TestMissingFilesKeepDefaults()
{
    g_test = "missing_files_keep_defaults";
    CleanDir();
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    CHECK(s == Settings{});
    CHECK(store.LastError().empty());
}

void TestLoadReadsValuesAndClamps()
{
    g_test = "load_reads_values_and_clamps";
    CleanDir();
    WriteFileBytes(ParryIni(),
        "[autoparry]\r\nparry=0\r\nmaster=0\r\npredict_ms=99999\r\npredict_dist=-3\r\nevade_ms=abc\r\nproj_lead_ms=120\r\nqte_log=0\r\n");
    WriteFileBytes(EspIni(), "[esp]\r\nradius=1000\r\nscale=0.1\r\nfov=v\r\nshow_raven=0\r\nenabled=0\r\n");
    WriteFileBytes(AppIni(), "[trainer]\r\nmenu_key=36\r\nauto_inject=0\r\n");
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    CHECK(s.parry.parry == false);
    CHECK(s.parry.master == false);
    CHECK(s.parry.predictMs == range::kPredictMs.max); // 越界 → 夹到上限
    CHECK(s.parry.predictDist == range::kPredictDist.min); // 负数 → 夹到下限
    CHECK(s.parry.evadeMs == 200);                         // 不是数字 → 用默认值
    CHECK(s.parry.projLeadMs == 120);
    CHECK(s.parry.qteLog == false);
    CHECK(s.parry.evade == true); // 文件里没有的键保持默认
    CHECK(s.esp.radius == range::kEspRadius.max);
    CHECK(s.esp.scale == range::kEspScale.min);
    CHECK(s.esp.fovHorizontal == false);
    CHECK(s.esp.show[7] == false); // raven
    CHECK(s.esp.show[0] == true);
    CHECK(s.esp.enabled == false);
    CHECK(s.app.menuKey == 36);
    CHECK(s.app.autoInject == false);
    CHECK(s.app.panicKey == VK_F12);
}

void TestToggleWritesImmediatelyAndKeepsComments()
{
    g_test = "toggle_writes_immediately_and_keeps_comments";
    CleanDir();
    WriteFileBytes(ParryIni(), kParryFile);
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    s.parry.parry = false;
    store.Tick(s, 1000);
    const std::string after = ReadFileBytes(ParryIni());
    CHECK(Contains(after, "parry=0"));
    CHECK(!Contains(after, "parry=1"));
    CHECK(Contains(after, "; 自动弹反 / 自动闪避配置，DLL 每秒重读，改完不用重启")); // 注释原样保留（UTF-8 字节没被转码）
    CHECK(Contains(after, "; 离出伤还剩多少毫秒时按 ×（越大闪得越早）"));
    CHECK(Contains(after, "predict_ms=350"));
    CHECK(!Contains(after, "master")); // 没改过的键不往文件里加
    CHECK(!Contains(after, "evade=")); // 同上（evade_ms= 不算）
    CHECK(store.LastError().empty());
}

void TestNumbersAreDebounced()
{
    g_test = "numbers_are_debounced";
    CleanDir();
    WriteFileBytes(ParryIni(), kParryFile);
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    s.parry.predictMs = 400;
    store.Tick(s, 1000);
    CHECK(Contains(ReadFileBytes(ParryIni()), "predict_ms=350")); // 刚改：先不写
    s.parry.predictMs = 420;                                      // 还在拖
    store.Tick(s, 1200);
    CHECK(Contains(ReadFileBytes(ParryIni()), "predict_ms=350"));
    store.Tick(s, 1400); // 距最后一次变化 200 ms：还不到 300
    CHECK(Contains(ReadFileBytes(ParryIni()), "predict_ms=350"));
    store.Tick(s, 1501); // 停了 301 ms：写
    CHECK(Contains(ReadFileBytes(ParryIni()), "predict_ms=420"));
}

void TestFlushWritesPending()
{
    g_test = "flush_writes_pending";
    CleanDir();
    WriteFileBytes(ParryIni(), kParryFile);
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    s.parry.evadeMs = 180;
    store.Tick(s, 1000);
    CHECK(Contains(ReadFileBytes(ParryIni()), "evade_ms=200"));
    store.Flush(s);
    CHECK(Contains(ReadFileBytes(ParryIni()), "evade_ms=180"));
}

void TestExternalEditIsReloaded()
{
    g_test = "external_edit_is_reloaded";
    CleanDir();
    WriteFileBytes(ParryIni(), kParryFile);
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    CHECK(store.Tick(s, 1000) == false); // 没人动过
    WriteFileBytes(ParryIni(), "[autoparry]\r\nparry=0\r\npredict_ms=500\r\n");
    BumpWriteTime(ParryIni(), 5);
    CHECK(store.Tick(s, 1500) == false); // 每秒才查一次
    CHECK(s.parry.parry == true);
    CHECK(store.Tick(s, 2100) == true);
    CHECK(s.parry.parry == false);
    CHECK(s.parry.predictMs == 500);
    // 重读进来的值不算「用户改动」，不该再写回去
    const std::string before = ReadFileBytes(ParryIni());
    store.Tick(s, 2200);
    store.Tick(s, 3300);
    CHECK(ReadFileBytes(ParryIni()) == before);
}

void TestOwnWriteIsNotTreatedAsExternal()
{
    g_test = "own_write_is_not_treated_as_external";
    CleanDir();
    WriteFileBytes(ParryIni(), kParryFile);
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    s.parry.evade = false;
    store.Tick(s, 1000);
    CHECK(store.Tick(s, 2100) == false); // 自己刚写的，不算外部改动
    CHECK(s.parry.evade == false);
}

void TestEspAndAppRoundTrip()
{
    g_test = "esp_and_app_round_trip";
    CleanDir();
    Settings s;
    {
        SettingsStore store = MakeStore();
        store.Load(s);
        s.esp.show[13] = false; // bell
        s.esp.radius = 85.0f;
        s.esp.scale = 1.25f;
        s.esp.fovHorizontal = false;
        s.esp.master = false;
        s.app.panicKey = VK_F9;
        s.app.unloadOnExit = false;
        store.Flush(s);
    }
    const std::string esp = ReadFileBytes(EspIni());
    CHECK(Contains(esp, "[esp]"));
    CHECK(Contains(esp, "show_bell=0"));
    CHECK(Contains(esp, "radius=85"));
    CHECK(Contains(esp, "scale=1.25"));
    CHECK(Contains(esp, "fov=v"));
    CHECK(Contains(esp, "master=0"));
    CHECK(Contains(ReadFileBytes(AppIni()), "[trainer]"));
    Settings loaded;
    SettingsStore again = MakeStore();
    again.Load(loaded);
    CHECK(loaded == s);
}

void TestWriteFailureSetsErrorAndDoesNotCrash()
{
    g_test = "write_failure_sets_error";
    Settings s;
    SettingsStore store(g_dir + L"no_such_dir\\", g_dir + L"no_such_dir\\trainer.ini", TestEvents());
    store.Load(s);
    CHECK(s == Settings{});
    s.parry.parry = false;
    store.Tick(s, 1000);
    CHECK(!store.LastError().empty());
    store.Tick(s, 1100); // 不反复重试刷屏、不崩
}

void TestReloadEventIsSignaledAfterWrite()
{
    g_test = "reload_event_is_signaled_after_write";
    CleanDir();
    const HANDLE parryEvent = CreateEventW(nullptr, TRUE, FALSE, g_parryEvent.c_str());
    const HANDLE espEvent = CreateEventW(nullptr, TRUE, FALSE, g_espEvent.c_str());
    Settings s;
    SettingsStore store = MakeStore();
    store.Load(s);
    s.parry.qteMash = false;
    store.Tick(s, 1000);
    CHECK(WaitForSingleObject(parryEvent, 0) == WAIT_OBJECT_0); // 写了弹反的 ini → 叫弹反 DLL 重读
    CHECK(WaitForSingleObject(espEvent, 0) == WAIT_TIMEOUT);    // 透视的没动 → 不叫
    s.esp.enabled = false;
    store.Tick(s, 1100);
    CHECK(WaitForSingleObject(espEvent, 0) == WAIT_OBJECT_0);
    CloseHandle(parryEvent);
    CloseHandle(espEvent);
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const DWORD pid = GetCurrentProcessId();
    g_dir = std::wstring(temp) + L"gow4_trainer_tests_" + std::to_wstring(pid) + L"\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_parryEvent = L"Local\\gow4_test_parry_reload_" + std::to_wstring(pid);
    g_espEvent = L"Local\\gow4_test_esp_reload_" + std::to_wstring(pid);

    TestMissingFilesKeepDefaults();
    TestLoadReadsValuesAndClamps();
    TestToggleWritesImmediatelyAndKeepsComments();
    TestNumbersAreDebounced();
    TestFlushWritesPending();
    TestExternalEditIsReloaded();
    TestOwnWriteIsNotTreatedAsExternal();
    TestEspAndAppRoundTrip();
    TestWriteFailureSetsErrorAndDoesNotCrash();
    TestReloadEventIsSignaledAfterWrite();

    CleanDir();
    RemoveDirectoryW(g_dir.c_str());
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
