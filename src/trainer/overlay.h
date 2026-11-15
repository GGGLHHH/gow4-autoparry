#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "imgui.h"

// 透明、置顶、鼠标穿透的覆盖窗口，用 D3D11 + Dear ImGui 画。
// 游戏要开「无边框窗口」模式，独占全屏会把覆盖层盖掉。
namespace overlay {

bool Create();
void Destroy();

// 处理窗口消息；收到 WM_QUIT 返回 false
bool PumpMessages();

// 对齐到游戏窗口的客户区；target 为空时铺满主显示器（没开游戏也能调 UI）。位置没变不碰 Z 序
void FollowWindow(HWND target);

// 每帧调用：游戏窗口跑到我们上面了才重新置顶
void KeepAbove(HWND target);

// 隐藏/显示覆盖层窗口。菜单和提示都不画的时候要隐藏：置顶透明窗口盖在游戏上会让系统多做一层合成
void SetVisible(bool visible);
bool Visible();

// 打开菜单时要接收鼠标键盘：去掉穿透并抢前台；关菜单时恢复穿透
void SetInteractive(bool interactive);

// 覆盖层自己是不是前台窗口（菜单开着时切到别的程序 → 不是）
bool IsForeground();

// SetForegroundWindow 在后台进程里经常不生效，借前台线程的输入队列绕过去
void BringToFront(HWND hwnd);

ImVec2 Size();

// 每帧最先调用：等到垂直同步再画
void WaitForVBlank();

void BeginFrame();
void EndFrame();

// 开发用：不开窗口，离屏画几帧存成 PNG（纯 CPU 渲染，游戏开着也不占显卡）。
// width / height 按 100% 缩放写，实际乘主显示器的 DPI 缩放，和真窗口里看到的一样大
bool RenderOffscreen(int width, int height, const std::function<void()>& draw, const wchar_t* pngPath, std::string& error);

} // namespace overlay
