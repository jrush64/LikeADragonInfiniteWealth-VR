#pragma once

#include <dxgi1_6.h>
#include <windows.h>

namespace LAD8VR::D3DCapture
{
// Called by the wrappers. Nothing here installs a hook: the wrapper objects are the hook.
void OnSwapChainCreated(IUnknown* deviceOrQueue, IDXGISwapChain1* swapchain) noexcept;
void OnPresent() noexcept;
void OnResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) noexcept;
void OnSetColorSpace(int colorSpace) noexcept;
void OnResizeResult(const char* what, HRESULT hr) noexcept;

void Shutdown() noexcept;
}
