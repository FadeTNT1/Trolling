#include "overlay.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <iostream>

// Forward-declare the ImGui Win32 message handler.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static Overlay* g_overlay = nullptr;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────
bool Overlay::Init(HINSTANCE hInstance)
{
    g_overlay = this;

    // ── Register window class ────────────────────────────────────────
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = L"WD42Overlay";
    RegisterClassExW(&wc);

    // ── Create overlay window ────────────────────────────────────────
    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"WD-42 Overlay",
        WS_POPUP,
        0, 0, 800, 600,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        std::cerr << "[overlay] CreateWindowEx failed\n";
        return false;
    }

    // Make the window fully transparent via DWM glass.
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Set layered window to use alpha; 255 = fully opaque layered surface.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // ── DX11 device + swap chain ─────────────────────────────────────
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 1;
    sd.BufferDesc.Width                   = 800;
    sd.BufferDesc.Height                  = 600;
    sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swapChain, &device, &featureLevel, &context
    );

    if (FAILED(hr)) {
        std::cerr << "[overlay] D3D11CreateDeviceAndSwapChain failed: 0x"
                  << std::hex << hr << "\n";
        return false;
    }

    // ── Render target view ───────────────────────────────────────────
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();

    // ── ImGui ────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    std::cout << "[overlay] Initialized (DX11 + ImGui)\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────
bool Overlay::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            running = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────
void Overlay::BeginFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

// ─────────────────────────────────────────────────────────────────────
void Overlay::EndFrame()
{
    ImGui::Render();

    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain->Present(1, 0);   // vsync on
}

// ─────────────────────────────────────────────────────────────────────
void Overlay::MatchWindow(const RECT& target)
{
    int w = target.right  - target.left;
    int h = target.bottom - target.top;
    if (w <= 0 || h <= 0) return;

    SetWindowPos(hwnd, HWND_TOPMOST,
                 target.left, target.top, w, h,
                 SWP_NOACTIVATE);
}

// ─────────────────────────────────────────────────────────────────────
void Overlay::ToggleInteraction()
{
    clickThrough = !clickThrough;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (clickThrough)
        exStyle |=  WS_EX_TRANSPARENT;
    else
        exStyle &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    std::cout << "[overlay] Click-through: " << (clickThrough ? "ON" : "OFF") << "\n";
}

// ─────────────────────────────────────────────────────────────────────
void Overlay::Shutdown()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (rtv)       { rtv->Release();       rtv       = nullptr; }
    if (swapChain) { swapChain->Release();  swapChain = nullptr; }
    if (context)   { context->Release();    context   = nullptr; }
    if (device)    { device->Release();     device    = nullptr; }
    if (hwnd)      { DestroyWindow(hwnd);   hwnd      = nullptr; }
}
