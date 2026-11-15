#pragma once

#include <windows.h>

inline bool KeyDown(int vk)
{
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// 全局快捷键的边沿检测：只在按下的那一帧返回 true（游戏在前台时也能收到）
class KeyEdge {
public:
    bool Pressed(int vk)
    {
        const bool down = KeyDown(vk);
        // 刚换成这个键（菜单里改键、程序刚启动）：正按着的这一下不算
        const bool pressed = down && !down_ && vk == vk_;
        down_ = down;
        vk_ = vk;
        return pressed;
    }

private:
    bool down_ = false;
    int vk_ = 0;
};
