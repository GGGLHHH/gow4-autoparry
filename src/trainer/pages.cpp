// 菜单四页的画法。只改 ctx.s 里的值、往 ctx.action 里放动作；读写 ini、注入卸载都不在这里做。

#include "menu.h"

#include "common/shared.h"
#include "widgets.h"

namespace ui {
namespace {

// 一行 DLL 状态：圆点 + 名字 + 状态文字 + 靠右的「注入 / 卸载」按钮
void DllRow(MenuContext& ctx, const char* name, const DllStatus& dll, Action inject, Action unload)
{
    const float scale = Scale();
    const float rowStart = ImGui::GetCursorPosX();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = 76.0f * scale;

    StatusDot(dll.injected ? kGreen : kMuted);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine(rowStart + 84.0f * scale);
    if (dll.injected)
        ImGui::Text("已注入%s%s", dll.version.empty() ? "" : "  ", dll.version.c_str());
    else
        ImGui::TextDisabled("未注入");

    ImGui::SameLine(rowStart + rowWidth - buttonWidth);
    ImGui::PushID(name);
    const bool canInject = ctx.status.gameRunning && ctx.status.versionOk;
    ImGui::BeginDisabled(!dll.injected && !canInject);
    if (ImGui::Button(dll.injected ? "卸载" : "注入", ImVec2(buttonWidth, 0.0f)))
        ctx.action = dll.injected ? unload : inject;
    ImGui::EndDisabled();
    ImGui::PopID();
}

} // namespace

void DrawOverviewPage(MenuContext& ctx)
{
    const RuntimeStatus& st = ctx.status;
    const float rowStart = ImGui::GetCursorPosX();

    StatusDot(!st.gameRunning ? kMuted : (st.versionOk ? kGreen : (st.versionRead ? kRed : kAmber)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("游戏");
    ImGui::SameLine(rowStart + 84.0f * Scale());
    if (!st.gameRunning)
        ImGui::TextDisabled("GoW.exe 没在运行");
    else if (st.versionOk)
        ImGui::Text("GoW.exe 已连接 · v1.0.13（进程 %lu）", st.pid);
    else if (st.versionRead)
        ImGui::TextColored(kRed, "GoW.exe 不是 v1.0.13，不注入");
    else
        ImGui::TextColored(kAmber, "GoW.exe 还读不到（刚启动等几秒；一直这样就用管理员身份开修改器）");

    DllRow(ctx, "弹反", st.parry, Action::InjectParry, Action::UnloadParry);
    DllRow(ctx, "透视", st.esp, Action::InjectEsp, Action::UnloadEsp);
    ErrorText(st.error);
    if ((st.parry.injected && st.parry.foreign) || (st.esp.injected && st.esp.foreign)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("游戏里的 DLL 不是这个修改器装的（从别的文件夹注入的）：它读的是那边的配置，这里改的设置对它不起作用。先卸载，再在这里注入。");
        ImGui::PopStyleColor();
    } else if ((st.parry.injected && !st.parry.reloadable) || (st.esp.injected && !st.esp.reloadable)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("游戏里是旧版 DLL：开关要 1～2 秒才生效，总开关和透视的逐类开关用不了。卸载后重新注入就是新版。");
        ImGui::PopStyleColor();
    }

    SectionTitle("功能");
    ParrySettings& p = ctx.s.parry;
    const bool paused = !p.master; // 总开关关着：各项的值照常能改，只是暂不生效，画暗一点
    BeginDim(paused);
    ToggleSwitch("自动弹反", &p.parry, "正面来的、盾接得住的攻击自动判成弹反。你自己按着格挡键时不干预");
    ToggleSwitch("自动闪避", &p.evade, "背后来的、红光（不可格挡）、盾接不了的攻击：替你闪开，来不及就吞掉这一下再闪");
    bool projectile = p.projPre || p.projEvade;
    if (ToggleSwitch("飞行物弹反 / 侧闪", &projectile, "盾接得住的飞行物提前举盾弹回去；接不了的、背后来的提前侧闪。两项可在「战斗」页分开设"))
        p.projPre = p.projEvade = projectile;
    ToggleSwitch("QTE 自动完成", &p.qteMash, "开宝箱、抬东西、挣脱这类「连按某键」的进度条自己涨满");
    ToggleSwitch("透视", &ctx.s.esp.enabled, "把附近的可收集物标在游戏画面上；已经拿走的不标");
    EndDim(paused);
}

void DrawCombatPage(MenuContext& ctx)
{
    ParrySettings& p = ctx.s.parry;

    SectionTitle("弹反");
    ToggleSwitch("自动弹反", &p.parry, "正面来的、盾接得住的攻击自动判成弹反。你自己按着格挡键时不干预");

    SectionTitle("闪避");
    ToggleSwitch("自动闪避", &p.evade, "背后来的、红光（不可格挡）、盾接不了的攻击：替你闪开，来不及就吞掉这一下再闪");
    ToggleSwitch("预判闪避", &p.predict, "按敌人招式的时间轴，在出伤之前提前闪，不等碰到");
    BeginDim(!p.predict);
    SliderRow("预判提前量", &p.predictMs, range::kPredictMs.min, range::kPredictMs.max, 10, "%d ms", "离出伤还剩多少毫秒时闪，越大闪得越早");
    SliderRow("预判距离", &p.predictDist, range::kPredictDist.min, range::kPredictDist.max, 1, "%d m", "敌人在这个距离以内才预判");
    EndDim(!p.predict);
    ToggleSwitch("招式预读", &p.preread, "敌人一出招就从招式数据里读出每一段攻击，红光 / 盾接不住的提前闪。关了就回到「碰到才闪」");
    ToggleSwitch("大范围下砸往侧面闪", &p.evadeSide, "巨魔这类能打到 4～5 米的下砸，往后闪躲不开，所以往侧面闪。关了改成往后闪");
    ToggleSwitch("闪后还被碰到时吞掉", &p.predSwallow, "预判闪避之后还是被碰到：开 = 吞掉，不掉血不受击；关 = 交给游戏自己判，可能挨打");
    SliderRow("闪避键按住", &p.evadeMs, range::kEvadeMs.min, range::kEvadeMs.max, 10, "%d ms", "替你按闪避键按多久。手按实测 150～200 毫秒");

    SectionTitle("飞行物");
    ToggleSwitch("飞行物弹反", &p.projPre, "按飞行物的轨迹算还有多久打到你，提前举盾，让游戏自己把它弹回去");
    BeginDim(!p.projPre);
    SliderRow("弹反提前量", &p.projLeadMs, range::kProjLeadMs.min, range::kProjLeadMs.max, 10, "%d ms", "离打到你还剩多少毫秒时举盾。举盾后约 140 毫秒弹反窗口才打开");
    EndDim(!p.projPre);
    ToggleSwitch("接不了的提前侧闪", &p.projEvade, "盾接不了的、从背后来的飞行物，到点往离开弹道的方向闪。关了就回到「碰到才吞掉再闪」");
    BeginDim(!p.projEvade);
    SliderRow("侧闪提前量", &p.projEvadeMs, range::kProjEvadeMs.min, range::kProjEvadeMs.max, 10, "%d ms", "离打到你还剩多少毫秒时闪");
    EndDim(!p.projEvade);

    SectionTitle("QTE");
    ToggleSwitch("连按 QTE 自动完成", &p.qteMash, "开宝箱、抬东西、挣脱这类「连按某键」的进度条自己涨满。个别游戏标了不许自动的仍要手按");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("实验（默认不用动）")) {
        ToggleSwitch("大范围下砸用翻滚", &p.evadeRoll, "连按两下闪避键翻滚，挪得更远。实测 5 次只翻出 1 次，默认关");
        BeginDim(!p.evadeRoll);
        SliderRow("翻滚：第一下按住", &p.rollTap1, range::kRollTap.min, range::kRollTap.max, 10, "%d ms");
        SliderRow("翻滚：中间松开", &p.rollGap, range::kRollGap.min, range::kRollGap.max, 10, "%d ms");
        SliderRow("翻滚：第二下按住", &p.rollTap2, range::kRollTap.min, range::kRollTap.max, 10, "%d ms");
        SliderRow("翻滚预判提前量", &p.predictRollMs, range::kPredictRollMs.min, range::kPredictRollMs.max, 10, "%d ms",
            "翻滚要等第二下才接上，所以比预判提前量大");
        EndDim(!p.evadeRoll);
        SliderRow("背后判定角度", &p.backDeg, range::kBackDeg.min, range::kBackDeg.max, 5, "%d°", "攻击来的方位超过这个角度就算背后，走闪避");
    }
}

void DrawEspPage(MenuContext& ctx)
{
    EspSettings& e = ctx.s.esp;

    ToggleSwitch("透视", &e.enabled, "把附近的可收集物标在游戏画面上；已经拿走的不标");
    const bool off = !e.enabled;
    BeginDim(off);
    // 滑块到 200 m 为止；ini 里手写更大的值（最大 500）照样认
    SliderRow("显示半径", &e.radius, range::kEspRadius.min, e.radius > 200.0f ? range::kEspRadius.max : 200.0f, 5.0f, "%.0f m",
        "只标这个距离以内的");
    SliderRow("标签大小", &e.scale, range::kEspScale.min, range::kEspScale.max, 0.05f, "%.2f 倍");
    static const char* const kFovModes[] = { "水平", "垂直" };
    int fovMode = e.fovHorizontal ? 0 : 1;
    if (SegmentedRow("视角按", &fovMode, kFovModes, 2, "标签和物品的位置对不上时换另一个"))
        e.fovHorizontal = fovMode == 0;

    SectionTitle("标哪些");
    // 诺恩宝箱和它的三种机关排在一起
    static const int kOrder[gow4::kEspKindCount] = { 0, 1, 2, 12, 3, 4, 13, 14, 5, 6, 7, 8, 9, 10, 11 };
    if (ImGui::BeginTable("kinds", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (const int k : kOrder) {
            const gow4::EspKind& kind = gow4::kEspKinds[k];
            ImGui::TableNextColumn();
            ColorSwatch(IM_COL32(kind.r, kind.g, kind.b, 255));
            ToggleSwitch(kind.label, &e.show[k], nullptr, 150.0f);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("全部显示"))
        for (bool& show : e.show)
            show = true;
    ImGui::SameLine();
    if (ImGui::Button("全部隐藏"))
        for (bool& show : e.show)
            show = false;
    EndDim(off);
}

void DrawSettingsPage(MenuContext& ctx)
{
    AppSettings& a = ctx.s.app;
    ParrySettings& p = ctx.s.parry;

    SectionTitle("按键");
    KeyBinder("打开 / 关闭菜单", a.menuKey);
    KeyBinder("总开关", a.panicKey);
    KeyBinder("退出修改器", a.exitKey);

    SectionTitle("启动和退出");
    ToggleSwitch("发现游戏时自动注入", &a.autoInject, "每个游戏进程只自动注入一次；之后手动卸载了，不会再自动注入");
    ToggleSwitch("退出时卸载", &a.unloadOnExit, "退出修改器时，把它自己注入的 DLL 卸掉；用命令行注入的不碰");

    SectionTitle("诊断");
    ToggleSwitch("QTE 记录", &p.qteLog, "只记录按键判定，不改任何结果。写到 gow4_qte.log");
    ToggleSwitch("飞行物轨迹诊断", &p.projDiag, "逐条记录飞行物的扫掠，日志会很多，只在调飞行物预判时开");
    ToggleSwitch("招式快照", &p.snap, "招式系统逐帧快照写到 gow4_ms_snap.bin，调试用");
    ToggleSwitch("弹反不限命中区域", &p.zone, "调试用。开了背后的攻击会变成转身格挡");

    ImGui::Spacing();
    if (ImGui::Button("打开日志文件夹"))
        ctx.action = Action::OpenLogs;
    ImGui::SameLine();
    if (ImGui::Button("退出修改器"))
        ctx.action = Action::Exit;
}

} // namespace ui
