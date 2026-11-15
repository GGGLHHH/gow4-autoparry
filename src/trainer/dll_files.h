#pragma once

// 两个注入 DLL 的文件在哪，以及它们的 ini（和修改器自己的 ini）放在哪个目录。
//   开发版：都在 exe 旁边（build\Release），DLL 是单独编出来的文件。
//   便携版（GOW4_PORTABLE，build.ps1 -Dist）：DLL 编在 exe 里，启动时释放到 %LOCALAPPDATA%\gow4_trainer\，ini 也放那里。
// DLL 只认自己旁边那份 ini，所以 ini 的目录必须跟着 DLL 走。

#include <string>

struct DllFiles {
    std::wstring dir;   // 末尾带反斜杠
    std::wstring parry; // 完整路径
    std::wstring esp;
    std::string error;  // 便携版释放失败的原因；空 = 没问题
};

DllFiles LocateDlls();

// 这个文件是不是在 dir 这个目录里（比较前都换成长路径，不分大小写）
bool InDirectory(const std::wstring& file, const std::wstring& dir);
