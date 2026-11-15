#pragma once

// 往游戏里装 / 卸 DLL。两个 DLL 加载后各自建命名事件（见 common/shared.h）：
// 「卸载」事件在不在 = 装没装上，置位 = 让它自己卸掉。

#include <windows.h>

#include <string>

namespace injector {

// 这个名字的事件在不在（DLL 建的，DLL 卸了就没了）
bool EventExists(const wchar_t* name);

// 置位事件；事件不在返回 false
bool SignalEvent(const wchar_t* name);

// 远程线程调 LoadLibraryW。失败时 error 里是原因
bool Inject(DWORD pid, const std::wstring& dllPath, std::string& error);

} // namespace injector
