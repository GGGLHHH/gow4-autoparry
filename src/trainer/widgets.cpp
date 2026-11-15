#include "widgets.h"

#include <windows.h>

#include <cfloat>
#include <cstdio>
#include <cstring>

namespace ui {

std::string ToUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string KeyName(int vk)
{
    switch (vk) {
    case 0: return "无";
    case VK_LBUTTON: return "鼠标左键";
    case VK_RBUTTON: return "鼠标右键";
    case VK_MBUTTON: return "鼠标中键";
    case VK_XBUTTON1: return "鼠标侧键1";
    case VK_XBUTTON2: return "鼠标侧键2";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_CAPITAL: return "CapsLock";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    }
    char buf[16];
    if (vk >= VK_F1 && vk <= VK_F24)
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        snprintf(buf, sizeof(buf), "%c", vk);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        snprintf(buf, sizeof(buf), "小键盘%d", vk - VK_NUMPAD0);
    else
        snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

bool ToggleSwitch(const char* label, bool* v, const char* tooltip, float labelWidthAt100)
{
    const float scale = Scale();
    const float height = ImGui::GetFrameHeight() * 0.78f;
    const float width = height * 1.9f;
    const float labelWidth = labelWidthAt100 * scale;
    ImGui::PushID(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float rowHeight = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton("##toggle", ImVec2(labelWidth + width, rowHeight));
    if (clicked)
        *v = !*v;
    const bool hovered = ImGui::IsItemHovered();
    if (tooltip)
        ImGui::SetItemTooltip("%s", tooltip);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4* colors = ImGui::GetStyle().Colors;
    // 「##」后面的是 ID，不显示（文字会变、ID 不能变的开关用，如「总开关（F12）###master」）
    const char* labelEnd = strstr(label, "##");
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
    draw->AddText(ImVec2(start.x, start.y + (rowHeight - textSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd);

    const ImVec2 p0(start.x + labelWidth, start.y + (rowHeight - height) * 0.5f);
    const ImVec2 p1(p0.x + width, p0.y + height);
    const ImVec4 off = hovered ? colors[ImGuiCol_FrameBgHovered] : colors[ImGuiCol_FrameBg];
    const ImVec4 on = hovered ? colors[ImGuiCol_SliderGrabActive] : colors[ImGuiCol_CheckMark];
    draw->AddRectFilled(p0, p1, ImGui::GetColorU32(*v ? on : off), height * 0.5f);
    const float r = height * 0.5f - 2.0f * scale;
    const float knobX = *v ? p1.x - height * 0.5f : p0.x + height * 0.5f;
    draw->AddCircleFilled(ImVec2(knobX, p0.y + height * 0.5f), r, ImGui::GetColorU32(ImVec4(0.96f, 0.97f, 0.98f, 1.0f)));
    ImGui::PopID();
    return clicked;
}

namespace {

// 一行的左半边：文字 + 提示，然后把光标挪到标签列之后（和 ToggleSwitch 的开关对齐）
void RowLabel(const char* label, const char* tooltip)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip)
        ImGui::SetItemTooltip("%s", tooltip);
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + kLabelWidth * Scale());
}

// 细轨道 + 圆钮的滑块（ImGui 自带的滑块把数值写在滑钮底下，会被压住）。t 是 0～1 的位置；返回 true = 正在拖
bool SlimSlider(const char* id, float* t, float width)
{
    const float scale = Scale();
    const float h = ImGui::GetFrameHeight();
    const float r = 7.0f * scale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const float x0 = p.x + r, x1 = p.x + width - r;
    if (active)
        *t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
    *t = *t < 0.0f ? 0.0f : (*t > 1.0f ? 1.0f : *t);

    const float cx = x0 + *t * (x1 - x0), cy = p.y + h * 0.5f, half = 2.0f * scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(x0, cy - half), ImVec2(x1, cy + half), ImGui::GetColorU32(ImGuiCol_FrameBgHovered), half);
    draw->AddRectFilled(ImVec2(x0, cy - half), ImVec2(cx, cy + half),
        ImGui::GetColorU32(active || hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab), half);
    draw->AddCircleFilled(ImVec2(cx, cy), active ? r + scale : r, ImGui::GetColorU32(ImVec4(0.96f, 0.97f, 0.98f, 1.0f)));
    return active;
}

// 滑块右边留给数值的宽度
float ValueWidth()
{
    return 76.0f * Scale();
}

} // namespace

bool SliderRow(const char* label, int* v, int min, int max, int step, const char* format, const char* tooltip)
{
    ImGui::PushID(label);
    RowLabel(label, tooltip);
    const int before = *v;
    float t = static_cast<float>(*v - min) / static_cast<float>(max - min);
    if (SlimSlider("##v", &t, ImGui::GetContentRegionAvail().x - ValueWidth())) {
        const int snapped = min + static_cast<int>(t * static_cast<float>(max - min) / static_cast<float>(step) + 0.5f) * step;
        *v = snapped < min ? min : (snapped > max ? max : snapped);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(format, *v);
    ImGui::PopID();
    return *v != before;
}

bool SliderRow(const char* label, float* v, float min, float max, float step, const char* format, const char* tooltip)
{
    ImGui::PushID(label);
    RowLabel(label, tooltip);
    const float before = *v;
    float t = (*v - min) / (max - min);
    if (SlimSlider("##v", &t, ImGui::GetContentRegionAvail().x - ValueWidth())) {
        const float snapped = min + static_cast<float>(static_cast<int>(t * (max - min) / step + 0.5f)) * step;
        *v = snapped < min ? min : (snapped > max ? max : snapped);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(format, *v);
    ImGui::PopID();
    return *v != before;
}

bool SegmentedRow(const char* label, int* index, const char* const* options, int count, const char* tooltip)
{
    const float scale = Scale();
    ImGui::PushID(label);
    RowLabel(label, tooltip);
    const int before = *index;
    const ImVec4* colors = ImGui::GetStyle().Colors;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f * scale, ImGui::GetStyle().ItemSpacing.y));
    for (int i = 0; i < count; ++i) {
        const bool selected = *index == i;
        ImGui::PushStyleColor(ImGuiCol_Button, colors[selected ? ImGuiCol_ButtonActive : ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[selected ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered]);
        if (ImGui::Button(options[i], ImVec2(88.0f * scale, 0.0f)))
            *index = i;
        ImGui::PopStyleColor(2);
        if (i + 1 < count)
            ImGui::SameLine();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return *index != before;
}

void BeginDim(bool dim)
{
    if (dim)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
}

void EndDim(bool dim)
{
    if (dim)
        ImGui::PopStyleVar();
}

void SectionTitle(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "%s", text);
    ImGui::Separator();
}

namespace {
const char* waitingFor = nullptr; // 正在等按键的那个绑定项（标签指针）
}

bool KeyBinderActive()
{
    return waitingFor != nullptr;
}

void KeyBinder(const char* label, int& vk, bool optional)
{
    static bool waitRelease = false;           // 点按钮的那下左键还没松开，先不收键
    static const char* suppressClick = nullptr; // 用左键绑定后，松开那一下会被按钮当成点击，忽略掉
    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + kLabelWidth * Scale());

    if (waitingFor == label) {
        ImGui::Button("请按键…  (Esc 取消)");
        if (waitRelease && !leftDown)
            waitRelease = false;
        for (int key = VK_LBUTTON; key <= 0xFE && !waitRelease; ++key) {
            if (!(GetAsyncKeyState(key) & 0x8000))
                continue;
            if (key != VK_ESCAPE)
                vk = key;
            waitingFor = nullptr;
            suppressClick = label;
            break;
        }
    } else if (ImGui::Button(KeyName(vk).c_str(), ImVec2(120.0f * Scale(), 0.0f)) && suppressClick != label) {
        waitingFor = label;
        waitRelease = true;
    }
    if (suppressClick == label && !leftDown)
        suppressClick = nullptr;
    if (optional && vk && waitingFor != label) {
        ImGui::SameLine();
        if (ImGui::SmallButton("清除"))
            vk = 0;
    }
    ImGui::PopID();
}

void StatusDot(const ImVec4& color)
{
    const float h = ImGui::GetFrameHeight();
    const float r = 4.5f * Scale();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(r * 2.0f, h));
    ImGui::SameLine();
}

void ColorSwatch(ImU32 color)
{
    const float h = ImGui::GetFrameHeight();
    const float w = 4.0f * Scale();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + w, p.y + h * 0.8f),
        ImGui::GetColorU32(color), w * 0.5f);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::SameLine();
}

void ErrorText(const std::string& text)
{
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void HintText(const char* text)
{
    if (!text || !*text)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace ui
