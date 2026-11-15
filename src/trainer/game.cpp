#include "game.h"

#include <tlhelp32.h>

#include <cstring>

#include "common/shared.h"

namespace game {
namespace {

DWORD FindPid()
{
    DWORD pid = 0;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, gow4::kGameExe) == 0)
            pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

struct WindowSearch {
    DWORD pid;
    HWND best;
    long bestArea;
};

// 这个进程名下最大的那个可见顶层窗口（游戏还会开几个看不见的小窗口）
BOOL CALLBACK OnWindow(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const long area = rect.right * rect.bottom;
    if (area > search->bestArea) {
        search->best = hwnd;
        search->bestArea = area;
    }
    return TRUE;
}

HWND FindMainWindow(DWORD pid)
{
    WindowSearch search{ pid, nullptr, 0 };
    EnumWindows(OnWindow, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

// 读主模块里版本核对点的 16 个字节，和 v1.0.13 的比。读不到（刚启动、没权限）也算不对，下一秒再试；read 说明读没读到
bool CheckVersion(DWORD pid, ULONGLONG& ageMs, bool& read)
{
    read = false;
    const HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc)
        return false;

    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        const ULONGLONG start = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const ULONGLONG current = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        ageMs = current > start ? (current - start) / 10000 : 0;
    }

    bool ok = false;
    // 模块快照的第一个就是 exe 自己
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        unsigned char bytes[sizeof(gow4::kVersionBytes)];
        SIZE_T got = 0;
        if (Module32FirstW(snap, &me)) {
            // 主模块比核对点还小：肯定不是这个游戏，算「读到了，对不上」
            read = me.modBaseSize <= gow4::kVersionCheckRva + sizeof(bytes) ||
                (ReadProcessMemory(proc, me.modBaseAddr + gow4::kVersionCheckRva, bytes, sizeof(bytes), &got) && got == sizeof(bytes));
            ok = read && me.modBaseSize > gow4::kVersionCheckRva + sizeof(bytes) &&
                memcmp(bytes, gow4::kVersionBytes, sizeof(bytes)) == 0;
        }
        CloseHandle(snap);
    }
    CloseHandle(proc);
    return ok;
}

} // namespace

std::wstring ModulePath(DWORD pid, const wchar_t* prefix)
{
    std::wstring path;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return path;
    const size_t prefixLen = wcslen(prefix);
    MODULEENTRY32W me{ sizeof(me) };
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsnicmp(me.szModule, prefix, prefixLen) == 0) {
            path = me.szExePath;
            break;
        }
    }
    CloseHandle(snap);
    return path;
}

const Info& Finder::Refresh()
{
    const DWORD pid = FindPid();
    if (pid != info_.pid) {
        info_ = Info{};
        info_.pid = pid;
    }
    if (!pid)
        return info_;

    if (!info_.window || !IsWindow(info_.window))
        info_.window = FindMainWindow(pid);

    // 核对通过的不重复读内存，只更新运行时长
    if (versionCheckedPid_ == pid) {
        info_.ageMs += 1000;
    } else {
        info_.versionOk = CheckVersion(pid, info_.ageMs, info_.versionRead);
        if (info_.versionOk)
            versionCheckedPid_ = pid;
    }
    return info_;
}

} // namespace game
