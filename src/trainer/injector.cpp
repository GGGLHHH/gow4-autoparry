#include "injector.h"

namespace injector {

bool EventExists(const wchar_t* name)
{
    const HANDLE e = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!e)
        return false;
    CloseHandle(e);
    return true;
}

bool SignalEvent(const wchar_t* name)
{
    const HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!e)
        return false;
    SetEvent(e);
    CloseHandle(e);
    return true;
}

bool Inject(DWORD pid, const std::wstring& dllPath, std::string& error)
{
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "找不到 DLL 文件";
        return false;
    }
    const HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!proc) {
        error = "打不开游戏进程（游戏是管理员身份开的话，修改器也要用管理员身份开）";
        return false;
    }
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    error = "写入 DLL 路径失败";
    if (remote && WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        const auto loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        error = "创建远程线程失败";
        if (const HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr)) {
            DWORD code = 0;
            ok = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && code;
            error = ok ? "" : "LoadLibrary 失败或超时";
            CloseHandle(thread);
        }
    }
    if (remote)
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

} // namespace injector
