#include "settings.h"

#include <cstddef>
#include <cwchar>
#include <vector>

namespace {

enum class Kind { Bool, Int, Float, Fov };

// 一个设置项：在哪个文件、叫什么键、什么类型、在 Settings 里的位置、取值范围。
// 读、写、比较都走这张表，加一项设置只要在这里加一行
struct Field {
    int file; // 0 = gow4_autoparry.ini，1 = gow4_esp.ini，2 = trainer.ini
    std::wstring key;
    Kind kind;
    size_t offset;
    double min = 0.0, max = 0.0; // Int / Float 用
};

constexpr int kFileCount = 3;
constexpr const wchar_t* kSections[kFileCount] = { gow4::kParrySection, gow4::kEspSection, L"trainer" };

Field Bool(int file, const wchar_t* key, size_t offset)
{
    return { file, key, Kind::Bool, offset };
}

Field Int(int file, const wchar_t* key, size_t offset, IntRange r)
{
    return { file, key, Kind::Int, offset, static_cast<double>(r.min), static_cast<double>(r.max) };
}

Field Float(int file, const wchar_t* key, size_t offset, FloatRange r)
{
    return { file, key, Kind::Float, offset, static_cast<double>(r.min), static_cast<double>(r.max) };
}

const std::vector<Field>& Fields()
{
    static const std::vector<Field> fields = [] {
        constexpr IntRange kKey{ 0, 0xFE }; // 虚拟键码
        std::vector<Field> f = {
            // gow4_autoparry.ini —— 键名和 DLL 里读的一致
            Bool(0, L"master", offsetof(Settings, parry.master)),
            Bool(0, L"parry", offsetof(Settings, parry.parry)),
            Bool(0, L"evade", offsetof(Settings, parry.evade)),
            Int(0, L"evade_ms", offsetof(Settings, parry.evadeMs), range::kEvadeMs),
            Bool(0, L"predict", offsetof(Settings, parry.predict)),
            Int(0, L"predict_ms", offsetof(Settings, parry.predictMs), range::kPredictMs),
            Int(0, L"predict_dist", offsetof(Settings, parry.predictDist), range::kPredictDist),
            Bool(0, L"evade_side", offsetof(Settings, parry.evadeSide)),
            Bool(0, L"preread", offsetof(Settings, parry.preread)),
            Bool(0, L"pred_swallow", offsetof(Settings, parry.predSwallow)),
            Bool(0, L"proj_pre", offsetof(Settings, parry.projPre)),
            Int(0, L"proj_lead_ms", offsetof(Settings, parry.projLeadMs), range::kProjLeadMs),
            Bool(0, L"proj_evade", offsetof(Settings, parry.projEvade)),
            Int(0, L"proj_evade_ms", offsetof(Settings, parry.projEvadeMs), range::kProjEvadeMs),
            Bool(0, L"qte_mash", offsetof(Settings, parry.qteMash)),
            Bool(0, L"evade_roll", offsetof(Settings, parry.evadeRoll)),
            Int(0, L"roll_tap1", offsetof(Settings, parry.rollTap1), range::kRollTap),
            Int(0, L"roll_gap", offsetof(Settings, parry.rollGap), range::kRollGap),
            Int(0, L"roll_tap2", offsetof(Settings, parry.rollTap2), range::kRollTap),
            Int(0, L"predict_roll_ms", offsetof(Settings, parry.predictRollMs), range::kPredictRollMs),
            Int(0, L"back_deg", offsetof(Settings, parry.backDeg), range::kBackDeg),
            Bool(0, L"proj_diag", offsetof(Settings, parry.projDiag)),
            Bool(0, L"qte_log", offsetof(Settings, parry.qteLog)),
            Bool(0, L"snap", offsetof(Settings, parry.snap)),
            Bool(0, L"zone", offsetof(Settings, parry.zone)),
            // gow4_esp.ini
            Bool(1, L"master", offsetof(Settings, esp.master)),
            Bool(1, L"enabled", offsetof(Settings, esp.enabled)),
            Float(1, L"radius", offsetof(Settings, esp.radius), range::kEspRadius),
            Float(1, L"scale", offsetof(Settings, esp.scale), range::kEspScale),
            { 1, L"fov", Kind::Fov, offsetof(Settings, esp.fovHorizontal) },
            // trainer.ini
            Int(2, L"menu_key", offsetof(Settings, app.menuKey), kKey),
            Int(2, L"panic_key", offsetof(Settings, app.panicKey), kKey),
            Int(2, L"exit_key", offsetof(Settings, app.exitKey), kKey),
            Bool(2, L"auto_inject", offsetof(Settings, app.autoInject)),
            Bool(2, L"unload_on_exit", offsetof(Settings, app.unloadOnExit)),
        };
        for (int i = 0; i < gow4::kEspKindCount; ++i)
            f.push_back(Bool(1, (std::wstring(L"show_") + gow4::kEspKinds[i].key).c_str(),
                offsetof(Settings, esp.show) + static_cast<size_t>(i) * sizeof(bool)));
        return f;
    }();
    return fields;
}

template <class T>
T& At(Settings& s, size_t offset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(&s) + offset);
}

template <class T>
const T& At(const Settings& s, size_t offset)
{
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&s) + offset);
}

bool Equal(const Field& f, const Settings& a, const Settings& b)
{
    switch (f.kind) {
    case Kind::Int: return At<int>(a, f.offset) == At<int>(b, f.offset);
    case Kind::Float: return At<float>(a, f.offset) == At<float>(b, f.offset);
    default: return At<bool>(a, f.offset) == At<bool>(b, f.offset);
    }
}

void Copy(const Field& f, const Settings& from, Settings& to)
{
    switch (f.kind) {
    case Kind::Int: At<int>(to, f.offset) = At<int>(from, f.offset); break;
    case Kind::Float: At<float>(to, f.offset) = At<float>(from, f.offset); break;
    default: At<bool>(to, f.offset) = At<bool>(from, f.offset); break;
    }
}

double Clamp(double v, double min, double max)
{
    return v < min ? min : (v > max ? max : v);
}

// 键不存在、值不是数字 → s 里的值不动
void Read(const Field& f, const std::wstring& path, Settings& s)
{
    wchar_t text[64];
    if (!GetPrivateProfileStringW(kSections[f.file], f.key.c_str(), L"", text, 64, path.c_str()))
        return;
    wchar_t* end = nullptr;
    switch (f.kind) {
    case Kind::Bool: {
        const long v = wcstol(text, &end, 10);
        if (end != text)
            At<bool>(s, f.offset) = v != 0;
        break;
    }
    case Kind::Int: {
        const long v = wcstol(text, &end, 10);
        if (end != text)
            At<int>(s, f.offset) = static_cast<int>(Clamp(static_cast<double>(v), f.min, f.max));
        break;
    }
    case Kind::Float: {
        const double v = wcstod(text, &end);
        if (end != text)
            At<float>(s, f.offset) = static_cast<float>(Clamp(v, f.min, f.max));
        break;
    }
    case Kind::Fov:
        At<bool>(s, f.offset) = text[0] != L'v' && text[0] != L'V';
        break;
    }
}

std::wstring Format(const Field& f, const Settings& s)
{
    wchar_t text[32];
    switch (f.kind) {
    case Kind::Int:
        swprintf(text, 32, L"%d", At<int>(s, f.offset));
        return text;
    case Kind::Float:
        swprintf(text, 32, L"%g", static_cast<double>(At<float>(s, f.offset)));
        return text;
    case Kind::Fov:
        return At<bool>(s, f.offset) ? L"h" : L"v";
    default:
        return At<bool>(s, f.offset) ? L"1" : L"0";
    }
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// 叫醒 DLL 立刻重读；事件不存在（没注入，或者是没有这个事件的旧版 DLL）就算了
void Signal(const wchar_t* name)
{
    if (const HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
        SetEvent(e);
        CloseHandle(e);
    }
}

} // namespace

SettingsStore::SettingsStore(std::wstring dllDir, std::wstring appIni, StoreEvents events) : events_(events)
{
    paths_[0] = dllDir + gow4::kParryIni;
    paths_[1] = dllDir + gow4::kEspIni;
    paths_[2] = std::move(appIni);
}

void SettingsStore::Load(Settings& s)
{
    for (int file = 0; file < kFileCount; ++file)
        LoadFile(file, s);
    saved_ = s;
    lastSeen_ = s;
}

void SettingsStore::LoadFile(int file, Settings& s)
{
    for (const Field& f : Fields()) {
        if (f.file != file)
            continue;
        Read(f, paths_[file], s);
        Copy(f, s, saved_);
    }
    writeTimes_[file] = WriteTime(file);
}

bool SettingsStore::Tick(Settings& s, ULONGLONG nowMs)
{
    if (s != lastSeen_) {
        lastSeen_ = s;
        changedAt_ = nowMs;
    }
    WriteChanged(s, nowMs - changedAt_ >= 300);

    if (nowMs - lastStat_ < 1000)
        return false;
    lastStat_ = nowMs;
    bool reloaded = false;
    for (int file = 0; file < kFileCount; ++file) {
        const FILETIME now = WriteTime(file);
        if (CompareFileTime(&now, &writeTimes_[file]) == 0)
            continue;
        // 这个文件还有没写出去的改动（正在拖滑块）：以用户手上的为准，等写出去之后再说
        bool pending = false;
        for (const Field& f : Fields())
            pending = pending || (f.file == file && !Equal(f, s, saved_));
        if (pending)
            continue;
        LoadFile(file, s);
        reloaded = true;
    }
    if (reloaded)
        lastSeen_ = s; // 重读进来的不算用户改动
    return reloaded;
}

void SettingsStore::Flush(const Settings& s)
{
    WriteChanged(s, true);
}

void SettingsStore::WriteChanged(const Settings& s, bool includeNumbers)
{
    bool wrote[kFileCount] = {};
    int failedFile = -1;
    DWORD failedCode = 0;
    for (const Field& f : Fields()) {
        if (Equal(f, s, saved_))
            continue;
        if ((f.kind == Kind::Int || f.kind == Kind::Float) && !includeNumbers)
            continue;
        if (WritePrivateProfileStringW(kSections[f.file], f.key.c_str(), Format(f, s).c_str(), paths_[f.file].c_str())) {
            wrote[f.file] = true;
        } else {
            failedFile = f.file;
            failedCode = GetLastError();
        }
        Copy(f, s, saved_); // 写失败也记成处理过了：不每帧重试，错误留在 LastError 里
    }

    for (int file = 0; file < kFileCount; ++file) {
        if (!wrote[file])
            continue;
        writeTimes_[file] = WriteTime(file); // 自己写的，不算外部改动
        if (file == 0)
            Signal(events_.parryReload);
        else if (file == 1)
            Signal(events_.espReload);
    }
    if (failedFile >= 0)
        error_ = "写不进 " + Utf8(paths_[failedFile]) + "（错误 " + std::to_string(failedCode) + "）";
    else if (wrote[0] || wrote[1] || wrote[2])
        error_.clear();
}

FILETIME SettingsStore::WriteTime(int file) const
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(paths_[file].c_str(), GetFileExInfoStandard, &info))
        return {};
    return info.ftLastWriteTime;
}
