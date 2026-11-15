#pragma once

// gow4_trainer.exe 和两个注入 DLL 共用的名字：文件名、ini 节 / 键、命名事件、透视的类别表。
// 只放常量，两边各自包含；改这里的名字要同时重编修改器和 DLL。

namespace gow4 {

inline constexpr wchar_t kGameExe[] = L"GoW.exe";

// 认版本：GoW.exe v1.0.13 在这个偏移上的 16 个字节（脚本接口 GetOutputFoV 的开头）。
// 两个 DLL 的钩子地址都是按这一版写死的，对不上就不注入
inline constexpr unsigned kVersionCheckRva = 0x7a6320;
inline constexpr unsigned char kVersionBytes[16] = {
    0x4C, 0x8B, 0x41, 0x10, 0x48, 0x8B, 0x05, 0xA5, 0x30, 0xB0, 0x01, 0x8B, 0x90, 0xF0, 0x08, 0x00};

inline constexpr wchar_t kParryDll[] = L"gow4_autoparry.dll";
inline constexpr wchar_t kEspDll[] = L"gow4_esp.dll";

// 配置文件放在 DLL 同目录，DLL 自己按这个名字找
inline constexpr wchar_t kParryIni[] = L"gow4_autoparry.ini";
inline constexpr wchar_t kEspIni[] = L"gow4_esp.ini";
inline constexpr wchar_t kParrySection[] = L"autoparry";
inline constexpr wchar_t kEspSection[] = L"esp";

// DLL 加载后创建；修改器靠「卸载事件在不在」判断是否已注入，置位 = 让 DLL 卸载
inline constexpr wchar_t kParryUnloadEvent[] = L"Local\\gow4_autoparry_unload";
inline constexpr wchar_t kEspUnloadEvent[] = L"Local\\gow4_esp_unload";
// 修改器写完 ini 后置位，DLL 立刻重读（没有这个事件的旧版 DLL 靠自己每 1～2 秒重读）
inline constexpr wchar_t kParryReloadEvent[] = L"Local\\gow4_autoparry_reload";
inline constexpr wchar_t kEspReloadEvent[] = L"Local\\gow4_esp_reload";

// 透视的类别。顺序 = esp.cpp 里 enum Kind 的顺序；ini 里的开关键名是 show_<key>
struct EspKind {
    const wchar_t* key;
    const char* label;
    unsigned char r, g, b;
};
inline constexpr int kEspKindCount = 15;
inline constexpr EspKind kEspKinds[kEspKindCount] = {
    {L"chest", "宝箱", 232, 180, 74},
    {L"rare", "稀有宝箱", 143, 168, 255},
    {L"legend", "传说", 255, 138, 61},
    {L"nornir", "诺恩宝箱", 179, 136, 255},
    {L"seal", "符文封印", 214, 190, 255},
    {L"silver", "银币", 201, 209, 217},
    {L"artifact", "古物", 79, 209, 197},
    {L"raven", "奥丁的乌鸦", 46, 229, 157},
    {L"lore", "碑文", 224, 201, 166},
    {L"shrine", "巨人神龛", 79, 195, 247},
    {L"map", "藏宝图", 215, 163, 90},
    {L"rune", "符文", 240, 98, 146},
    {L"bag", "战利品", 165, 184, 74},
    {L"bell", "符文铃铛", 214, 190, 255},
    {L"spinner", "符文转盘", 214, 190, 255},
};

} // namespace gow4
