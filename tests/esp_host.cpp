// 透视 DLL 的测试宿主：开一个 DX11 窗口持续出帧（Present / Present1 交替），加载 gow4_esp.dll（自测模式），
// 跑 N 秒后从屏幕抓窗口客户区存成 BMP，再发卸载事件，确认 DLL 能干净卸掉、卸掉后继续出帧不崩。不碰游戏。
// 用法：esp_host.exe <dll路径> <截图.bmp> [秒数=3] [flip=0|1]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "gdi32.lib")

static void CaptureClient(HWND hwnd, const wchar_t* path)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT tl{0, 0};
    ClientToScreen(hwnd, &tl);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);
    BITMAPINFOHEADER ih{sizeof(ih), w, h, 1, 32, BI_RGB};
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    GetDIBits(mem, bmp, 0, h, px.data(), reinterpret_cast<BITMAPINFO*>(&ih), DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    if (FILE* f = _wfopen(path, L"wb")) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(px.data(), px.size(), 1, f);
        fclose(f);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return 2;
    const int secs = argc > 3 ? _wtoi(argv[3]) : 3;
    const bool flip = argc > 4 && _wtoi(argv[4]) == 1;
    SetEnvironmentVariableW(L"GOW4_ESP_SELFTEST", L"1");
    SetProcessDPIAware();

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"esp_host";
    RegisterClassW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"esp_host", L"esp_host", WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc = {1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &sd, &sc, &dev, nullptr, &ctx))) {
        puts("建设备失败");
        return 1;
    }
    IDXGISwapChain1* sc1 = nullptr;
    sc->QueryInterface(IID_PPV_ARGS(&sc1));

    HMODULE dll = LoadLibraryW(argv[1]);
    printf("LoadLibrary %s\n", dll ? "成功" : "失败");
    if (!dll)
        return 1;

    auto frame = [&](int i) {
        ID3D11Texture2D* bb = nullptr;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        ID3D11RenderTargetView* rtv = nullptr;
        dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        const float clear[4] = {0.18f, 0.22f, 0.20f, 1.f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        DXGI_PRESENT_PARAMETERS pp{};
        const HRESULT hr = (i & 1) && sc1 ? sc1->Present1(1, 0, &pp) : sc->Present(1, 0);
        // 宿主自己的渲染目标要还在：确认 DLL 恢复了 OM 状态
        ID3D11RenderTargetView* cur = nullptr;
        ctx->OMGetRenderTargets(1, &cur, nullptr);
        const bool restored = cur == rtv;
        if (cur)
            cur->Release();
        rtv->Release();
        Pump();
        return SUCCEEDED(hr) && restored;
    };

    int frames = 0, bad = 0;
    for (const ULONGLONG end = GetTickCount64() + secs * 1000; GetTickCount64() < end; ++frames)
        bad += !frame(frames);
    printf("出帧 %d，Present 失败或渲染目标没恢复 %d 次\n", frames, bad);
    CaptureClient(hwnd, argv[2]);

    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\gow4_esp_unload");
    printf("卸载事件 %s\n", e ? "找到" : "没找到");
    if (e) {
        SetEvent(e);
        CloseHandle(e);
    }
    int i = 0;
    for (; i < 300 && GetModuleHandleW(L"gow4_esp.dll"); ++i) // 卸载期间继续出帧，让渲染线程自己关 ImGui
        frame(i);
    puts(GetModuleHandleW(L"gow4_esp.dll") ? "DLL 没卸掉！" : "DLL 已卸载");
    printf("（卸载用了 %d 帧）\n", i);
    for (int k = 0; k < 30; ++k)
        frame(k);
    puts("卸载后出帧正常");
    if (sc1)
        sc1->Release();
    ctx->Release();
    sc->Release();
    dev->Release();
    return 0;
}
