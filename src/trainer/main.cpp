// 战神4 辅助的界面：外部程序，透明覆盖层上画菜单。功能本身在注入游戏的两个 DLL 里（gow4_autoparry / gow4_esp），
// 这里只负责：找游戏、注入 / 卸载、把菜单里的设置写进 DLL 的 ini。设计见 docs/plan.md。
//
// 开发用命令行：
//   gow4_trainer.exe --shot <文件.png> [页号 0-4] [样例 0-2]   不开窗口，离屏画一帧存成图片
//     页号：0 总览，1 战斗，2 透视，3 设置，4 = 只画左上角的提示条
//     样例：0 = 游戏没开，1 = 都已注入（新版 DLL），2 = 游戏里是旧版 DLL + 总开关关着
//   gow4_trainer.exe --check <文件.txt>   只看不动的自检：读配置、找游戏、看 DLL 装没装，结果写进文件

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/shared.h"
#include "dll_files.h"
#include "game.h"
#include "hotkeys.h"
#include "injector.h"
#include "menu.h"
#include "overlay.h"
#include "settings.h"
#include "widgets.h"

namespace {

constexpr ULONGLONG kToastMs = 1500;           // 提示条显示多久
constexpr ULONGLONG kToastFadeMs = 300;        // 其中最后这么久淡出
constexpr ULONGLONG kAutoInjectAfterMs = 15000; // 游戏进程起来这么久之后才自动注入（等它把窗口和渲染建好）
constexpr ULONGLONG kUnloadSettleMs = 3000;    // 让 DLL 卸载之后这么久之内不再注入同一个（它还在收尾）

// 一个注入 DLL
struct Dll {
    const char* name;            // 界面和报错里叫什么
    const wchar_t* modulePrefix; // 游戏里模块名的开头（便携版释放出来的文件名后面带哈希）
    std::wstring path;           // 要注入的 DLL 文件
    const wchar_t* unloadEvent;
    const wchar_t* reloadEvent;
    DWORD pathCheckedPid = 0;    // 已经查过「游戏里那份是从哪加载的」的进程
    bool foreign = false;        // 游戏里那份不在我们的目录里
    DWORD seenPid = 0;        // 在哪个游戏进程里见过它装着：见过就不再自动注入（手动卸了不会自己装回去）
    DWORD injectedPid = 0;    // 修改器自己注入进了哪个进程（退出时只卸这些）
    ULONGLONG busyUntil = 0;  // 刚让它卸载，这之前不注入
};

std::vector<std::wstring> CommandLineArgs()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 0; argv && i < argc; ++i)
        args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

struct Toast {
    std::string text;
    ImVec4 color;
    ULONGLONG until = 0;
};

void DrawToastIfAny(const Toast& toast, ULONGLONG now)
{
    if (now >= toast.until)
        return;
    const ULONGLONG left = toast.until - now;
    const float alpha = left >= kToastFadeMs ? 1.0f : static_cast<float>(left) / static_cast<float>(kToastFadeMs);
    ui::DrawToast(toast.text, toast.color, alpha);
}

// 切总开关时提示条上写什么
Toast MasterToast(const Settings& settings, const ui::RuntimeStatus& status, ULONGLONG now)
{
    const bool oldDll = (status.parry.injected && !status.parry.reloadable) || (status.esp.injected && !status.esp.reloadable);
    const std::string key = ui::KeyName(settings.app.panicKey);
    if (oldDll)
        return { "游戏里是旧版 DLL，总开关不起作用（卸载后重新注入）", ui::kRed, now + kToastMs * 2 };
    if (settings.parry.master)
        return { "辅助已恢复", ui::kGreen, now + kToastMs };
    return { "辅助已暂停（" + key + " 恢复）", ui::kAmber, now + kToastMs };
}

void OpenLogs()
{
    wchar_t temp[MAX_PATH];
    const DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len >= MAX_PATH)
        return;
    const std::wstring log = std::wstring(temp, len) + L"gow4_autoparry.log";
    if (GetFileAttributesW(log.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring args = L"/select,\"" + log + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", temp, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

int RunShot(const std::vector<std::wstring>& args)
{
    Settings settings;
    ui::RuntimeStatus status;
    const int page = args.size() > 3 ? _wtoi(args[3].c_str()) : 0;
    const int sample = args.size() > 4 ? _wtoi(args[4].c_str()) : 1;
    if (sample >= 1) {
        status.gameRunning = status.versionOk = true;
        status.pid = 27628;
        status.parry = { true, sample == 1, sample == 1 ? "v6.24" : "" };
        status.esp = { true, sample == 1, "" };
    }
    if (sample == 2)
        settings.parry.master = settings.esp.master = false;

    std::string error;
    const bool ok = overlay::RenderOffscreen(620, page == 4 ? 90 : 1000,
        [&] {
            if (page == 4) {
                const Toast toast = MasterToast(settings, status, 0);
                ui::DrawToast(toast.text, toast.color, 1.0f);
                return;
            }
            ui::MenuContext ctx{ settings, status };
            ctx.forceTab = page;
            ui::DrawMenu(ctx);
        },
        args[2].c_str(), error);
    return ok ? 0 : 1;
}

// 只看不动的自检：读配置、找游戏、看 DLL 装没装、用真显卡设备在不显示的窗口上画几帧菜单，结果写进文本文件。
// 不注入、不卸载、不写 ini、不认快捷键、不显示任何窗口
int RunCheck(const std::wstring& reportPath)
{
    const DllFiles files = LocateDlls();
    Settings settings;
    SettingsStore store(files.dir, files.dir + L"gow4_trainer.ini");
    store.Load(settings);

    game::Finder finder;
    const game::Info info = finder.Refresh();
    ui::RuntimeStatus status;
    status.gameRunning = info.pid != 0;
    status.versionOk = info.versionOk;
    status.versionRead = info.versionRead;
    status.pid = info.pid;
    status.parry.injected = info.pid && injector::EventExists(gow4::kParryUnloadEvent);
    status.parry.reloadable = status.parry.injected && injector::EventExists(gow4::kParryReloadEvent);
    status.esp.injected = info.pid && injector::EventExists(gow4::kEspUnloadEvent);
    status.esp.reloadable = status.esp.injected && injector::EventExists(gow4::kEspReloadEvent);
    const std::wstring parryLoaded = status.parry.injected ? game::ModulePath(info.pid, L"gow4_autoparry") : L"";
    const std::wstring espLoaded = status.esp.injected ? game::ModulePath(info.pid, L"gow4_esp") : L"";
    status.parry.foreign = !parryLoaded.empty() && !InDirectory(parryLoaded, files.dir);
    status.esp.foreign = !espLoaded.empty() && !InDirectory(espLoaded, files.dir);

    int frames = 0;
    const bool overlayOk = overlay::Create();
    if (overlayOk) {
        for (; frames < 5; ++frames) {
            overlay::PumpMessages();
            overlay::BeginFrame();
            ui::MenuContext ctx{ settings, status };
            ui::DrawMenu(ctx);
            overlay::EndFrame();
        }
        overlay::Destroy();
    }

    RECT client{};
    if (info.window)
        GetClientRect(info.window, &client);
    const auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    char text[2048];
    const int len = snprintf(text, sizeof(text),
        "DLL 和配置的目录：%s%s%s\r\n弹反 DLL 文件：%s\r\n透视 DLL 文件：%s\r\n"
        "覆盖层 %s，画了 %d 帧\r\n"
        "配置：弹反=%d 闪避=%d 预判提前量=%d 飞行物弹反=%d QTE=%d ｜ 透视半径=%.0f 标签=%.2f ｜ 菜单键=%d 总开关键=%d 自动注入=%d\r\n"
        "游戏：进程 %lu，版本%s，窗口 %p（%ld x %ld），已运行 %llu 秒\r\n"
        "弹反 DLL：%s%s%s %s\r\n透视 DLL：%s%s%s %s\r\n",
        ui::ToUtf8(files.dir).c_str(), files.error.empty() ? "" : "  出错：", files.error.c_str(),
        exists(files.parry) ? ui::ToUtf8(files.parry).c_str() : "没有", exists(files.esp) ? ui::ToUtf8(files.esp).c_str() : "没有",
        overlayOk ? "建好了" : "没建成", frames, settings.parry.parry, settings.parry.evade, settings.parry.predictMs,
        settings.parry.projPre, settings.parry.qteMash, settings.esp.radius, settings.esp.scale, settings.app.menuKey,
        settings.app.panicKey, settings.app.autoInject, info.pid, info.versionOk ? "对" : (info.versionRead ? "不对" : "没读到"),
        static_cast<void*>(info.window), client.right, client.bottom, info.ageMs / 1000,
        status.parry.injected ? "已注入" : "未注入", status.parry.reloadable ? "（新版）" : "",
        status.parry.foreign ? "（别处注入的）" : "", ui::ToUtf8(parryLoaded).c_str(),
        status.esp.injected ? "已注入" : "未注入", status.esp.reloadable ? "（新版）" : "",
        status.esp.foreign ? "（别处注入的）" : "", ui::ToUtf8(espLoaded).c_str());
    const HANDLE file = CreateFileW(reportPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(file);
    }
    return overlayOk ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::vector<std::wstring> args = CommandLineArgs();
    if (args.size() >= 3 && args[1] == L"--shot")
        return RunShot(args);
    if (args.size() >= 3 && args[1] == L"--check")
        return RunCheck(args[2]);

    // 只允许开一个：两个修改器同时写 ini、同时抢快捷键会互相打架
    const HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\gow4_trainer_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"战神4 辅助已经开着了（在游戏里按 Insert 打开菜单）", L"战神4 辅助", MB_ICONINFORMATION);
        return 0;
    }

    const DllFiles files = LocateDlls();
    Settings settings;
    SettingsStore store(files.dir, files.dir + L"gow4_trainer.ini");
    store.Load(settings);

    if (!overlay::Create()) {
        MessageBoxW(nullptr, L"覆盖层创建失败（D3D11 初始化不了）", L"战神4 辅助", MB_ICONERROR);
        return 1;
    }
    overlay::SetInteractive(false);

    Dll dlls[2] = {
        { "弹反", L"gow4_autoparry", files.parry, gow4::kParryUnloadEvent, gow4::kParryReloadEvent },
        { "透视", L"gow4_esp", files.esp, gow4::kEspUnloadEvent, gow4::kEspReloadEvent },
    };
    game::Finder finder;
    game::Info info;
    ui::RuntimeStatus status;
    status.error = files.error; // 便携版释放 DLL 失败的原因，空 = 没问题
    Toast toast;

    auto inject = [&](Dll& dll, ULONGLONG now) -> bool {
        if (!info.pid || !info.versionOk || now < dll.busyUntil)
            return false;
        std::string why;
        dll.seenPid = info.pid;
        if (injector::Inject(info.pid, dll.path, why)) {
            dll.injectedPid = info.pid;
            status.error.clear();
            return true;
        }
        status.error = std::string(dll.name) + "注入失败：" + why;
        return false;
    };
    auto unload = [&](Dll& dll, ULONGLONG now) {
        if (injector::SignalEvent(dll.unloadEvent)) {
            dll.busyUntil = now + kUnloadSettleMs;
            dll.injectedPid = 0;
        }
    };

    bool menuOpen = false;
    bool greeted = false;         // 启动后给过一次「我开着」的反馈了（菜单默认收着，不给反馈就像没启动）
    bool menuHadFocus = false;    // 这次打开之后覆盖层拿到过前台
    ULONGLONG menuLostFocusAt = 0;
    bool rebinding = false;       // 菜单里正在改键（或者刚改完、键还没松开）：这期间不认快捷键
    KeyEdge menuKey, panicKey, exitKey;
    ULONGLONG lastPoll = 0;
    bool quit = false;

    auto openMenu = [&] {
        menuOpen = true;
        menuHadFocus = false;
        menuLostFocusAt = 0;
        overlay::FollowWindow(info.window);
        overlay::SetVisible(true);
        overlay::SetInteractive(true);
    };
    // backToGame：把前台还给游戏（用户自己切到别的程序去的时候不抢）
    auto closeMenu = [&](bool backToGame) {
        menuOpen = false;
        overlay::SetInteractive(false);
        if (backToGame && info.window && IsWindow(info.window))
            overlay::BringToFront(info.window);
    };

    while (!quit && overlay::PumpMessages()) {
        const ULONGLONG now = GetTickCount64();
        store.Tick(settings, now);

        // 每秒一次：找游戏、看两个 DLL 装没装、该自动注入的注入
        if (lastPoll == 0 || now - lastPoll >= 1000) {
            lastPoll = now;
            const DWORD oldPid = info.pid;
            info = finder.Refresh();
            if (info.pid != oldPid)
                status.error.clear();
            status.gameRunning = info.pid != 0;
            status.versionOk = info.versionOk;
            status.versionRead = info.versionRead;
            status.pid = info.pid;
            const bool readyForAuto = settings.app.autoInject && info.pid && info.versionOk && info.window &&
                info.ageMs >= kAutoInjectAfterMs;
            bool autoTried = false;
            std::string autoError; // 自动注入里失败的那个的原因（后一个成功会把 status.error 清掉，这里另存一份）
            for (int i = 0; i < 2; ++i) {
                Dll& dll = dlls[i];
                ui::DllStatus& st = i == 0 ? status.parry : status.esp;
                st.injected = info.pid && injector::EventExists(dll.unloadEvent);
                st.reloadable = st.injected && injector::EventExists(dll.reloadEvent);
                if (st.injected) {
                    dll.seenPid = info.pid;
                    // 每个进程查一次：游戏里那份是不是从我们的目录加载的（不是的话它读的是别处的 ini）
                    if (dll.pathCheckedPid != info.pid) {
                        const std::wstring loaded = game::ModulePath(info.pid, dll.modulePrefix);
                        if (!loaded.empty()) {
                            dll.pathCheckedPid = info.pid;
                            dll.foreign = !InDirectory(loaded, files.dir);
                        }
                    }
                } else {
                    dll.pathCheckedPid = 0;
                    dll.foreign = false;
                    if (readyForAuto && dll.seenPid != info.pid && now >= dll.busyUntil) {
                        autoTried = true;
                        if (!inject(dll, now))
                            autoError = status.error;
                    }
                }
                st.foreign = st.injected && dll.foreign;
            }
            if (menuOpen)
                overlay::FollowWindow(info.window);

            // 菜单收着时的反馈：自动注入的结果；启动后第一次轮询说一声「我开着」
            const std::string menuKeyName = ui::KeyName(settings.app.menuKey);
            if (autoTried) {
                lastPoll = now - 800; // 0.2 秒后刷新一次状态
                if (!autoError.empty())
                    status.error = autoError;
                if (!menuOpen)
                    toast = autoError.empty() ? Toast{ "辅助已注入（" + menuKeyName + " 开菜单）", ui::kGreen, now + kToastMs * 2 }
                                              : Toast{ autoError + "（" + menuKeyName + " 开菜单看详情）", ui::kRed, now + kToastMs * 3 };
            }
            if (!greeted) {
                greeted = true;
                if (!info.pid)
                    openMenu(); // 游戏还没开：直接把菜单亮出来，看得到它启动了，也能先调设置
                else if (!autoTried)
                    toast = { "战神4 辅助已启动（" + menuKeyName + " 开菜单）", ui::kGreen, now + kToastMs * 2 };
            }
        }

        // 快捷键。只在游戏或者菜单在前台时认，免得在别的程序里按 F12 / End 误触；
        // 游戏没开时菜单键在哪都认（先开菜单调设置）
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        const bool hotkeysOn = (info.pid && foregroundPid == info.pid) || overlay::IsForeground();
        bool menuHit = menuKey.Pressed(settings.app.menuKey);
        bool panicHit = panicKey.Pressed(settings.app.panicKey);
        bool exitHit = exitKey.Pressed(settings.app.exitKey);
        if (ui::KeyBinderActive())
            rebinding = true;
        if (rebinding) {
            if (!ui::KeyBinderActive() && !KeyDown(settings.app.menuKey) && !KeyDown(settings.app.panicKey) &&
                !KeyDown(settings.app.exitKey))
                rebinding = false;
            menuHit = panicHit = exitHit = false;
        }
        if (menuHit && (hotkeysOn || menuOpen || !info.pid)) {
            if (menuOpen)
                closeMenu(true);
            else
                openMenu();
        }
        if (panicHit && hotkeysOn) {
            settings.parry.master = settings.esp.master = !settings.parry.master;
            if (!menuOpen) // 菜单开着时菜单上自己看得到
                toast = MasterToast(settings, status, now);
        }
        if (exitHit && hotkeysOn)
            break;

        // 菜单开着时用户切到别的程序去了：把菜单收起来，置顶的菜单不挡别人
        if (menuOpen) {
            if (overlay::IsForeground()) {
                menuHadFocus = true;
                menuLostFocusAt = 0;
            } else if (menuHadFocus) {
                if (!menuLostFocusAt)
                    menuLostFocusAt = now;
                else if (now - menuLostFocusAt > 300)
                    closeMenu(false);
            }
        }

        // 菜单和提示条都没有：覆盖层藏起来，只轮询快捷键（置顶透明窗口盖在游戏上会让系统多做一层合成）
        const bool toastOn = now < toast.until;
        if (!menuOpen && !toastOn) {
            if (overlay::Visible()) {
                overlay::BeginFrame(); // 先画一帧空的把画面清掉再藏，下次显示时不会闪一下旧内容
                overlay::EndFrame();
                overlay::SetVisible(false);
            }
            Sleep(15);
            continue;
        }
        if (!overlay::Visible()) {
            overlay::FollowWindow(info.window);
            overlay::SetVisible(true);
        }
        overlay::KeepAbove(info.window);

        overlay::WaitForVBlank();
        overlay::BeginFrame();
        ui::Action action = ui::Action::None;
        if (menuOpen) {
            ui::MenuContext ctx{ settings, status };
            ctx.storeError = store.LastError();
            ui::DrawMenu(ctx);
            action = ctx.action;
        } else {
            DrawToastIfAny(toast, now);
        }
        overlay::EndFrame();

        switch (action) {
        case ui::Action::InjectParry: inject(dlls[0], now); break;
        case ui::Action::InjectEsp: inject(dlls[1], now); break;
        case ui::Action::UnloadParry: unload(dlls[0], now); break;
        case ui::Action::UnloadEsp: unload(dlls[1], now); break;
        case ui::Action::OpenLogs: OpenLogs(); break;
        case ui::Action::Exit: quit = true; break;
        case ui::Action::None: break;
        }
        if (action != ui::Action::None && lastPoll)
            lastPoll = now - 800; // 0.2 秒后刷新一次状态，界面上马上看得到结果
    }

    store.Flush(settings);
    // 退出时只卸修改器自己注入的；用命令行注入的不碰
    if (settings.app.unloadOnExit)
        for (const Dll& dll : dlls)
            if (dll.injectedPid && dll.injectedPid == info.pid)
                injector::SignalEvent(dll.unloadEvent);

    overlay::Destroy();
    if (single)
        CloseHandle(single);
    return 0;
}
