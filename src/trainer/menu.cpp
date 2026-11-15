// 主菜单窗口：总开关 + 标签页 + 底部提示

#include "menu.h"

#include <cstdio>

#include "widgets.h"

namespace ui {

void DrawMenu(MenuContext& ctx)
{
    const float scale = Scale();
    ImGui::SetNextWindowSize(ImVec2(580.0f * scale, 0.0f), ImGuiCond_Always);
    if (ctx.forceTab >= 0)
        ImGui::SetNextWindowPos(ImVec2(16.0f * scale, 16.0f * scale), ImGuiCond_Always);
    else
        ImGui::SetNextWindowPos(ImVec2(80.0f * scale, 160.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("战神4 辅助", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    // 总开关：弹反和透视两边的 master 一起切
    bool master = ctx.s.parry.master;
    char label[64];
    snprintf(label, sizeof(label), "总开关（%s）###master", KeyName(ctx.s.app.panicKey).c_str());
    if (ToggleSwitch(label, &master, "一键全关 / 恢复。关掉时所有功能暂停，各项自己的开关不变，再开就回到原样"))
        ctx.s.parry.master = ctx.s.esp.master = master;
    if (!master) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, "已全部暂停");
    }

    struct Tab {
        const char* name;
        void (*draw)(MenuContext&);
    };
    static const Tab kTabs[kTabCount] = {
        { "总览", DrawOverviewPage },
        { "战斗", DrawCombatPage },
        { "透视", DrawEspPage },
        { "设置", DrawSettingsPage },
    };
    if (ImGui::BeginTabBar("tabs")) {
        for (int i = 0; i < kTabCount; ++i) {
            const ImGuiTabItemFlags flags = ctx.forceTab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(kTabs[i].name, nullptr, flags)) {
                // 总开关关着：战斗 / 透视两页的设置照常能改，只是暂不生效，整页画暗一点（总览页自己只暗「功能」那一段）
                const bool paused = !ctx.s.parry.master && (i == 1 || i == 2);
                BeginDim(paused);
                kTabs[i].draw(ctx);
                EndDim(paused);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ErrorText(ctx.storeError);
    ImGui::TextDisabled("改动自动保存 · %s 关闭菜单", KeyName(ctx.s.app.menuKey).c_str());

    ImGui::End();
}

void DrawToast(const std::string& text, const ImVec4& color, float alpha)
{
    const float scale = Scale();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 pad(14.0f * scale, 9.0f * scale);
    const float dot = 5.0f * scale;
    const float gap = 9.0f * scale;
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min(24.0f * scale, 24.0f * scale);
    const ImVec2 max(min.x + pad.x * 2.0f + dot * 2.0f + gap + textSize.x, min.y + pad.y * 2.0f + textSize.y);
    const float rounding = 8.0f * scale;

    dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.075f, 0.085f, 0.105f, 0.94f * alpha)), rounding);
    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.22f, 0.25f, 0.30f, 0.60f * alpha)), rounding);
    dl->AddCircleFilled(ImVec2(min.x + pad.x + dot, (min.y + max.y) * 0.5f), dot,
        ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, alpha)));
    dl->AddText(ImVec2(min.x + pad.x + dot * 2.0f + gap, min.y + pad.y), ImGui::GetColorU32(ImVec4(0.90f, 0.92f, 0.95f, alpha)),
        text.c_str());
}

} // namespace ui
