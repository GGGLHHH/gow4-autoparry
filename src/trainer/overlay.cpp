#include "overlay.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstring>
#include <vector>

#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay {
namespace {

constexpr wchar_t kClassName[] = L"Gow4TrainerOverlay";
constexpr LONG_PTR kPassThroughStyles = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;
RECT g_rect{};

// 游戏窗口是不是在我们上面（从我们往 Z 序上方找）
bool IsBelow(HWND target)
{
    for (HWND h = GetWindow(g_hwnd, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
        if (h == target)
            return true;
    return false;
}

void CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool CreateDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
    if (FAILED(hr))
        return false;

    // 默认最多排队 3 帧，菜单会比鼠标慢好几帧；只留 1 帧
    IDXGIDevice1* dxgiDevice = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        dxgiDevice->SetMaximumFrameLatency(1);
        dxgiDevice->Release();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDevice()
{
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

RECT PrimaryMonitorRect()
{
    return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

float PrimaryDpiScale()
{
    return ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
}

// 定制主题（不用 ImGui 默认的 StyleColorsDark）：深石墨底、青色强调、圆角、宽松间距
void ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(16.0f, 14.0f);
    s.FramePadding = ImVec2(10.0f, 6.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    const ImVec4 bg(0.075f, 0.085f, 0.105f, 0.96f);
    const ImVec4 panel(0.115f, 0.13f, 0.16f, 1.0f);
    const ImVec4 panelHi(0.16f, 0.18f, 0.22f, 1.0f);
    const ImVec4 accent(0.20f, 0.78f, 0.74f, 1.0f);
    const ImVec4 accentDim(0.14f, 0.52f, 0.50f, 1.0f);
    const ImVec4 text(0.90f, 0.92f, 0.95f, 1.0f);
    const ImVec4 textDim(0.52f, 0.57f, 0.64f, 1.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.125f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.25f, 0.30f, 0.60f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelHi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.115f, 0.14f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = panelHi;
    c[ImGuiCol_ScrollbarGrabHovered] = accentDim;
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.90f, 0.86f, 1.0f);
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = panelHi;
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = panel;
    c[ImGuiCol_HeaderHovered] = panelHi;
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.25f, 0.30f, 0.60f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = panelHi;
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.78f, 0.74f, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
}

// ImGui 上下文 + 主题 + 字体：真窗口和离屏出图共用，保证两边看到的一样
void SetupImGui(float scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // 窗口布局不落盘，配置统一走 ini
    io.LogFilename = nullptr;

    ApplyTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 17.0f;

    // 微软雅黑带中文；1.92 起字形按需加载，不用指定字符范围
    const char* cjkFont = "C:\\Windows\\Fonts\\msyh.ttc";
    if (GetFileAttributesA(cjkFont) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(cjkFont);
}

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// 32 位 BGRA（每行 width*4 字节，无填充）存成 PNG
bool SavePng(const wchar_t* path, const uint8_t* bgra, UINT width, UINT height)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT stride = width * 4;
    const bool ok =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgra))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);
    if (SUCCEEDED(init))
        CoUninitialize();
    return ok;
}

} // namespace

bool Create()
{
    // 物理像素坐标，和游戏的分辨率对得上
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = PrimaryDpiScale();

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, kClassName, nullptr };
    RegisterClassExW(&wc);

    g_rect = PrimaryMonitorRect();
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | kPassThroughStyles,
        kClassName, L"GoW4 Trainer Overlay", WS_POPUP,
        g_rect.left, g_rect.top, g_rect.right - g_rect.left, g_rect.bottom - g_rect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd)
        return false;

    // 整窗不透明度 255，透明靠 DWM 按像素 alpha 合成：清屏用 alpha=0 的颜色就是全透
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    if (!CreateDevice(g_hwnd)) {
        CleanupDevice();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(kClassName, wc.hInstance);
        return false;
    }

    // 先不显示：菜单打开或者要弹提示时才 SetVisible(true)
    SetupImGui(scale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    return true;
}

void Destroy()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice();
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT)
            return false;
    }
    return true;
}

void FollowWindow(HWND target)
{
    RECT rect = PrimaryMonitorRect();
    if (target && IsWindow(target) && !IsIconic(target)) {
        RECT client;
        GetClientRect(target, &client);
        POINT topLeft{ client.left, client.top };
        ClientToScreen(target, &topLeft);
        rect = RECT{ topLeft.x, topLeft.y, topLeft.x + client.right, topLeft.y + client.bottom };
    }

    // 位置没变就不碰 Z 序：置顶由 KeepAbove 负责，只在真的被压下去时才纠正
    if (EqualRect(&rect, &g_rect))
        return;
    g_rect = rect;
    SetWindowPos(g_hwnd, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
}

void KeepAbove(HWND target)
{
    if (IsWindowVisible(g_hwnd) && target && IsWindow(target) && IsBelow(target))
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
}

void SetVisible(bool visible)
{
    if (visible != Visible())
        ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool Visible()
{
    return IsWindowVisible(g_hwnd) != FALSE;
}

void SetInteractive(bool interactive)
{
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = interactive ? (ex & ~kPassThroughStyles) : (ex | kPassThroughStyles);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);

    ImGuiIO& io = ImGui::GetIO();
    if (interactive) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        BringToFront(g_hwnd);
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }
}

bool IsForeground()
{
    return GetForegroundWindow() == g_hwnd;
}

void BringToFront(HWND hwnd)
{
    const DWORD self = GetCurrentThreadId();
    const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (foreground && foreground != self)
        AttachThreadInput(self, foreground, TRUE);
    SetForegroundWindow(hwnd);
    if (foreground && foreground != self)
        AttachThreadInput(self, foreground, FALSE);
}

ImVec2 Size()
{
    return ImVec2(static_cast<float>(g_rect.right - g_rect.left), static_cast<float>(g_rect.bottom - g_rect.top));
}

void WaitForVBlank()
{
    if (FAILED(DwmFlush()))
        Sleep(1);
}

void BeginFrame()
{
    if (g_resizeWidth && g_resizeHeight) {
        CleanupRenderTarget();
        g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_resizeWidth = g_resizeHeight = 0;
        CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void EndFrame()
{
    ImGui::Render();
    const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
    g_context->ClearRenderTargetView(g_renderTarget, transparent);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(0, 0); // 节奏由帧首的 WaitForVBlank 控制，这里不再等
}

bool RenderOffscreen(int width, int height, const std::function<void()>& draw, const wchar_t* pngPath, std::string& error)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = PrimaryDpiScale();
    const UINT w = static_cast<UINT>(static_cast<float>(width) * scale);
    const UINT h = static_cast<UINT>(static_cast<float>(height) * scale);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    // WARP = 纯 CPU 渲染
    bool ok = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                  &level, &context)) &&
        SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
        SUCCEEDED(device->CreateRenderTargetView(target, nullptr, &view)) &&
        SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
    if (!ok) {
        error = "离屏渲染设备建不了（D3D11 WARP）";
    } else {
        SetupImGui(scale);
        ImGui_ImplDX11_Init(device, context);
        ImGuiIO& io = ImGui::GetIO();
        const float background[4] = { 0.23f, 0.25f, 0.27f, 1.0f }; // 中灰：看得出菜单的半透明底
        for (int frame = 0; frame < 4; ++frame) { // 多画几帧：窗口自动大小、标签页选中都要一两帧才稳定
            io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
            io.DeltaTime = 1.0f / 60.0f;
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            draw();
            ImGui::Render();
            context->OMSetRenderTargets(1, &view, nullptr);
            context->ClearRenderTargetView(view, background);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        context->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE map{};
        ok = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (!ok) {
            error = "读不回离屏画面";
        } else {
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
            for (UINT y = 0; y < h; ++y)
                memcpy(&pixels[static_cast<size_t>(y) * w * 4], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    static_cast<size_t>(w) * 4);
            context->Unmap(staging, 0);
            for (size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            ok = SavePng(pngPath, pixels.data(), w, h);
            if (!ok)
                error = "PNG 写不出去";
        }
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
    }
    SafeRelease(view);
    SafeRelease(staging);
    SafeRelease(target);
    SafeRelease(context);
    SafeRelease(device);
    return ok;
}

} // namespace overlay
