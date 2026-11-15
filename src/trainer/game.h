#pragma once

// 找游戏：GoW.exe 的进程、主窗口、是不是认得的版本。只读，不往游戏里写任何东西。

#include <windows.h>

#include <string>

namespace game {

// 游戏进程里名字以 prefix 开头的那个模块的完整路径（比如 gow4_esp → …\gow4_esp.dll）；没有就返回空
std::wstring ModulePath(DWORD pid, const wchar_t* prefix);

struct Info {
    DWORD pid = 0;          // 0 = 游戏没在运行
    HWND window = nullptr;  // 主窗口；刚启动还没出窗口时是空
    bool versionOk = false; // 版本核对点的 16 个字节对得上
    bool versionRead = false; // 那 16 个字节读到了（没读到 = 游戏刚启动，或者没权限读它的内存）
    ULONGLONG ageMs = 0;    // 进程已经跑了多久
};

class Finder {
public:
    // 主循环每秒调一次
    const Info& Refresh();

private:
    Info info_;
    DWORD versionCheckedPid_ = 0; // 已经核对通过的进程，不重复读
};

} // namespace game
