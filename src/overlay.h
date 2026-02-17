#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

struct Overlay {
    HWND                    hwnd            = nullptr;
    ID3D11Device*           device          = nullptr;
    ID3D11DeviceContext*    context         = nullptr;
    IDXGISwapChain*         swapChain       = nullptr;
    ID3D11RenderTargetView* rtv            = nullptr;
    bool                    clickThrough    = true;
    bool                    running         = true;

    // Create the transparent overlay window + DX11 + ImGui.
    bool Init(HINSTANCE hInstance);

    // Process Windows messages. Returns false when WM_QUIT is received.
    bool PumpMessages();

    // Begin a new ImGui frame.
    void BeginFrame();

    // End the frame: render ImGui draw data, present the swap chain.
    void EndFrame();

    // Reposition/resize the overlay to match the target window rect.
    void MatchWindow(const RECT& target);

    // Toggle click-through (INSERT key).
    void ToggleInteraction();

    // Release all resources.
    void Shutdown();
};
