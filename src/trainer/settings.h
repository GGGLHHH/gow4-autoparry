#pragma once

// 菜单里能改的全部设置。ParrySettings / EspSettings 和两个 DLL 的 ini 一一对应，AppSettings 是修改器自己的。

#include <windows.h>

#include <string>

#include "common/shared.h"

struct IntRange {
    int min, max;
};
struct FloatRange {
    float min, max;
};

// 取值范围：读 ini 时夹到这里面，界面上的滑块也用同一组
namespace range {
inline constexpr IntRange kEvadeMs{50, 400};
inline constexpr IntRange kPredictMs{100, 800};
inline constexpr IntRange kPredictDist{2, 15};
inline constexpr IntRange kProjLeadMs{60, 400};
inline constexpr IntRange kProjEvadeMs{100, 600};
inline constexpr IntRange kRollTap{50, 300};
inline constexpr IntRange kRollGap{100, 600};
inline constexpr IntRange kPredictRollMs{300, 1200};
inline constexpr IntRange kBackDeg{60, 180};
inline constexpr FloatRange kEspRadius{5.0f, 500.0f};
inline constexpr FloatRange kEspScale{0.5f, 3.0f};
} // namespace range

// gow4_autoparry.ini [autoparry]。默认值和 DLL 里读不到时用的默认值一致
struct ParrySettings {
    bool master = true; // 总开关：关掉时下面所有「替你出手」的功能按关处理，各自的值不变

    bool parry = true;
    bool evade = true;
    int evadeMs = 200;
    bool predict = true;
    int predictMs = 350;
    int predictDist = 6;
    bool evadeSide = true;
    bool preread = true;
    bool predSwallow = true;

    bool projPre = true;
    int projLeadMs = 160;
    bool projEvade = true;
    int projEvadeMs = 300;

    bool qteMash = true;

    // 实验
    bool evadeRoll = false;
    int rollTap1 = 120;
    int rollGap = 280;
    int rollTap2 = 150;
    int predictRollMs = 750;
    int backDeg = 100;

    // 诊断
    bool projDiag = false;
    bool qteLog = true;
    bool snap = false;
    bool zone = false;

    bool operator==(const ParrySettings&) const = default;
};

// gow4_esp.ini [esp]
struct EspSettings {
    bool master = true;  // 总开关（和弹反的一起由 F12 切）
    bool enabled = true; // 透视自己的开关
    float radius = 60.0f;
    float scale = 1.0f;
    bool fovHorizontal = true;
    bool show[gow4::kEspKindCount] = {true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};

    bool operator==(const EspSettings&) const = default;
};

// trainer.ini [trainer]
struct AppSettings {
    int menuKey = VK_INSERT;
    int panicKey = VK_F12;
    int exitKey = VK_END;
    bool autoInject = true;
    bool unloadOnExit = true;

    bool operator==(const AppSettings&) const = default;
};

struct Settings {
    ParrySettings parry;
    EspSettings esp;
    AppSettings app;

    bool operator==(const Settings&) const = default;
};

// 写完 ini 后要叫醒谁。默认是两个 DLL 的重读事件；测试里换成别的名字，免得叫醒游戏里真的 DLL
struct StoreEvents {
    const wchar_t* parryReload = gow4::kParryReloadEvent;
    const wchar_t* espReload = gow4::kEspReloadEvent;
};

// Settings ↔ 三个 ini（gow4_autoparry.ini、gow4_esp.ini、trainer.ini）。
// 两个 DLL 的 ini 是功能设置的唯一来源：这里只逐键写「用户改过的」，别的键、注释都不动；
// 文件被别人改了（手改、DLL 自己回写）就重读，界面跟着变。
class SettingsStore {
public:
    // dllDir：两个 DLL 的 ini 所在目录，末尾带反斜杠；appIni：trainer.ini 的完整路径
    SettingsStore(std::wstring dllDir, std::wstring appIni, StoreEvents events = {});

    // 读进 s。文件或键不存在、值不是数字 → 保持 s 里原来的值（默认值）；越界 → 夹到范围内
    void Load(Settings& s);

    // 每帧调一次：
    //   s 和文件里的不一样 → 开关 / 选项立刻写，数值等它停了 300 ms 再写（拖滑块时不每帧写盘），写完发重读事件；
    //   每秒查一次三个文件的修改时间，被别人改过就重读进 s。
    // 返回 true = 这次调用里 s 被外部改动刷新了
    bool Tick(Settings& s, ULONGLONG nowMs);

    // 把还没写的改动马上写掉（退出前调）
    void Flush(const Settings& s);

    // 最近一次写盘失败的原因；之后写成功了就清空
    const std::string& LastError() const { return error_; }

private:
    void LoadFile(int file, Settings& s);
    void WriteChanged(const Settings& s, bool includeNumbers);
    FILETIME WriteTime(int file) const;

    std::wstring paths_[3];
    StoreEvents events_;
    Settings saved_;      // 文件里现在的内容（读进来 / 写出去之后的样子）
    Settings lastSeen_;   // 上一帧的 s，用来判断「还在拖」
    ULONGLONG changedAt_ = 0;
    ULONGLONG lastStat_ = 0;
    FILETIME writeTimes_[3]{};
    std::string error_;
};
