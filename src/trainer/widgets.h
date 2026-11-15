#pragma once

// 各页共用的界面小件：颜色、拨动开关、滑块行、按键绑定、状态圆点……

#include <string>

#include "imgui.h"

namespace ui {

inline const ImVec4 kGreen(0.40f, 0.90f, 0.55f, 1.0f);
inline const ImVec4 kRed(1.0f, 0.45f, 0.45f, 1.0f);
inline const ImVec4 kAmber(1.0f, 0.75f, 0.3f, 1.0f);
inline const ImVec4 kMuted(0.5f, 0.5f, 0.55f, 1.0f);

// 标签列的宽度（100% 缩放下的像素）：开关和滑块都从这一列之后开始，上下对齐
inline constexpr float kLabelWidth = 200.0f;

// 当前 DPI 缩放（尺寸常量都按 100% 写，乘它）
inline float Scale()
{
    return ImGui::GetStyle().FontScaleDpi;
}

std::string ToUtf8(const std::wstring& s);
std::string KeyName(int vk);

// 拨动开关（代替复选框）：左边文字，右边药丸形状的开关，点文字或开关都能切换。返回 true 表示这一帧切换了
bool ToggleSwitch(const char* label, bool* v, const char* tooltip = nullptr, float labelWidth = kLabelWidth);

// 一行滑块：左边文字，中间细轨道滑块，右边数值（format 里带单位，如 "%d ms"）。拖动时按 step 取整。
// 返回 true 表示这一帧值变了
bool SliderRow(const char* label, int* v, int min, int max, int step, const char* format, const char* tooltip = nullptr);
bool SliderRow(const char* label, float* v, float min, float max, float step, const char* format, const char* tooltip = nullptr);

// 一行分段按钮（几选一，代替单选圆点）。返回 true 表示这一帧换了选项
bool SegmentedRow(const char* label, int* index, const char* const* options, int count, const char* tooltip = nullptr);

// 把接下来画的东西变暗（「总开关关着」「这项功能没开」时用），控件照常能点。成对调用
void BeginDim(bool dim);
void EndDim(bool dim);

// 小节标题：强调色文字 + 细分隔线
void SectionTitle(const char* text);

// 按键绑定：点一下按钮，再按想要的键（Esc 取消）。optional = 旁边多一个「清除」按钮，可以不绑
void KeyBinder(const char* label, int& vk, bool optional = false);
// 有绑定项正在等按键：这时按下的键是给它的，主循环不要当快捷键处理
bool KeyBinderActive();

// 行首的状态圆点 / 颜色块；画完接着在同一行写文字
void StatusDot(const ImVec4& color);
void ColorSwatch(ImU32 color);

// 非空才显示：红色错误 / 灰色提示（自动换行）
void ErrorText(const std::string& text);
void HintText(const char* text);

} // namespace ui
