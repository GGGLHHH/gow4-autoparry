// 注入 / 卸载 DLL。用法：gow4_inject.exe load [dll名]   |   gow4_inject.exe unload [名字，默认 gow4_probe]
// 卸载靠 DLL 自己等的命名事件 Local\<名字>_unload
// DLL 默认是 exe 同目录的 gow4_probe.dll
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>

static DWORD FindGame()
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"GoW.exe") == 0)
            pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv)
{
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"unload") {
        const wchar_t* name = argc > 2 ? argv[2] : L"gow4_probe";
        const std::wstring event = std::wstring(L"Local\\") + name + L"_unload";
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str());
        if (!e) {
            wprintf(L"%ls 没在运行\n", name);
            return 1;
        }
        SetEvent(e);
        CloseHandle(e);
        wprintf(L"已通知 %ls 卸载\n", name);
        return 0;
    }
    if (cmd != L"load") {
        puts("用法: gow4_inject load [dll名] | unload [名字]");
        return 2;
    }
    const DWORD pid = FindGame();
    if (!pid) {
        puts("GoW.exe 没在运行");
        return 1;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dll(path);
    dll.resize(dll.find_last_of(L"\\/") + 1);
    dll += argc > 2 ? argv[2] : L"gow4_probe.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"找不到 %ls\n", dll.c_str());
        return 1;
    }
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!proc) {
        puts("打不开游戏进程");
        return 1;
    }
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int rc = 1;
    if (remote && WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        auto loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (HANDLE t = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr)) {
            DWORD code = 0;
            if (WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code) {
                printf("已注入 pid=%lu\n", pid);
                rc = 0;
            } else {
                puts("LoadLibrary 失败或超时");
            }
            CloseHandle(t);
        }
    }
    if (remote)
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return rc;
}
