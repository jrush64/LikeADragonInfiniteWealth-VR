#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

namespace LAD8VR::Xr
{
// Brought up lazily from the first wrapped swapchain, since that is where the game's device
// and present queue first become visible.
void OnSwapChainReady(IUnknown* presentQueue, IDXGISwapChain1* swapchain) noexcept;

// Called from the wrapped swapchain's Present, before the real Present runs. Copies the
// backbuffer into the runtime's eye images and submits a frame.
void OnPresent(IDXGISwapChain1* swapchain) noexcept;

void OnResize() noexcept;
void Shutdown() noexcept;

bool Enabled() noexcept;
}
