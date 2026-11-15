#pragma once

// 菜单窗口。各页只改 Settings、往 MenuContext::action 里放「用户点了什么」，真正的注入 / 卸载 / 退出由主循环做。

#include <string>

#include "imgui.h"
#include "settings.h"

namespace ui {

struct DllStatus {
    bool injected = false;
    bool reloadable = false; // DLL 建了「重读」事件 = 支持总开关和即时生效（v6.24 起）
    std::string version;     // 状态块里读到的版本号，没有就空着
    bool foreign = false;    // 游戏里那份是从别的目录加载的（比如命令行注入的）：它读的是那边的 ini，这里的设置管不到
};

// 游戏和两个 DLL 现在的状态，主循环每秒更新一次
struct RuntimeStatus {
    bool gameRunning = false;
    bool versionOk = false;
    bool versionRead = false; // 版本核对点读到了；没读到 = 游戏刚启动或者没权限，不是「版本不对」
    unsigned long pid = 0;
    DllStatus parry;
    DllStatus esp;
    std::string error; // 最近一次注入 / 卸载失败的原因
};

enum class Action { None, InjectParry, UnloadParry, InjectEsp, UnloadEsp, OpenLogs, Exit };

struct MenuContext {
    Settings& s;
    const RuntimeStatus& status;
    std::string storeError;        // 配置写盘失败的原因
    int forceTab = -1;             // 离屏出图用：强制选中第几页，同时把窗口摆到左上角
    Action action = Action::None;  // 这一帧用户点了什么
};

inline constexpr int kTabCount = 4;

void DrawMenu(MenuContext& ctx);

// 屏幕左上角的一行提示（菜单关着时切总开关用）：圆点 + 文字。alpha 0～1，用来淡出
void DrawToast(const std::string& text, const ImVec4& color, float alpha);

// 各页（pages.cpp）
void DrawOverviewPage(MenuContext& ctx);
void DrawCombatPage(MenuContext& ctx);
void DrawEspPage(MenuContext& ctx);
void DrawSettingsPage(MenuContext& ctx);

} // namespace ui
