#include "dll_files.h"

#include <windows.h>

#include <cstdio>

#include "common/shared.h"

#ifdef GOW4_PORTABLE
// 由 cmake/embed.cmake 在构建时生成
extern const unsigned char kParryDllBlob[];
extern const unsigned long kParryDllBlob_size;
extern const unsigned char kEspDllBlob[];
extern const unsigned long kEspDllBlob_size;
#endif

namespace {

std::wstring LongPath(const std::wstring& path)
{
    wchar_t buf[MAX_PATH];
    const DWORD len = GetLongPathNameW(path.c_str(), buf, MAX_PATH);
    return len > 0 && len < MAX_PATH ? std::wstring(buf, len) : path;
}

#ifdef GOW4_PORTABLE

unsigned long long Fnv1a64(const unsigned char* p, unsigned long n)
{
    unsigned long long h = 1469598103934665603ull;
    for (unsigned long i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool WriteAll(const std::wstring& path, const unsigned char* data, unsigned long size)
{
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}

// 把内嵌的 DLL 释放成 <dir><stem>_<内容哈希>.dll，返回完整路径。
// 文件名带哈希：换了版本就是新文件，旧版还被游戏加载着也不冲突；同一版本已经释放过就直接用
std::wstring Extract(const std::wstring& dir, const wchar_t* stem, const unsigned char* data, unsigned long size, std::string& error)
{
    wchar_t name[96];
    swprintf(name, 96, L"%ls_%016llx.dll", stem, Fnv1a64(data, size));
    const std::wstring path = dir + name;

    // 清掉以前版本释放的（正被游戏加载着的删不掉，随它去）
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = dir + stem + L"_*.dll";
    const HANDLE find = FindFirstFileW(pattern.c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(found.cFileName, name) != 0)
                DeleteFileW((dir + found.cFileName).c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeHigh == 0 && info.nFileSizeLow == size)
        return path;

    // 先写临时文件再改名，免得半个文件被拿去注入
    const std::wstring tmp = path + L".tmp";
    if (!WriteAll(tmp, data, size) || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        DeleteFileW(tmp.c_str());
        error = "释放内嵌的 DLL 失败（错误 " + std::to_string(code) + "）";
        return {};
    }
    return path;
}

#else

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, len);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    return dir;
}

#endif

} // namespace

DllFiles LocateDlls()
{
    DllFiles files;
#ifdef GOW4_PORTABLE
    wchar_t base[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        len = GetTempPathW(MAX_PATH, base);
    if (len == 0 || len >= MAX_PATH) {
        files.error = "找不到能放 DLL 的目录（LOCALAPPDATA / TEMP 都没有）";
        return files;
    }
    files.dir.assign(base, len);
    if (files.dir.back() != L'\\')
        files.dir += L'\\';
    files.dir += L"gow4_trainer\\";
    CreateDirectoryW(files.dir.c_str(), nullptr);
    files.parry = Extract(files.dir, L"gow4_autoparry", kParryDllBlob, kParryDllBlob_size, files.error);
    files.esp = Extract(files.dir, L"gow4_esp", kEspDllBlob, kEspDllBlob_size, files.error);
#else
    files.dir = ExeDir();
    files.parry = files.dir + gow4::kParryDll;
    files.esp = files.dir + gow4::kEspDll;
#endif
    return files;
}

bool InDirectory(const std::wstring& file, const std::wstring& dir)
{
    const std::wstring longFile = LongPath(file);
    const std::wstring longDir = LongPath(dir);
    const size_t slash = longFile.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;
    std::wstring fileDir = longFile.substr(0, slash + 1);
    std::wstring wantDir = longDir;
    if (!wantDir.empty() && wantDir.back() != L'\\' && wantDir.back() != L'/')
        wantDir += L'\\';
    return _wcsicmp(fileDir.c_str(), wantDir.c_str()) == 0;
}
