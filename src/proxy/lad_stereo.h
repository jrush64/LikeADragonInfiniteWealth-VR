#pragma once

#include <d3d12.h>

namespace LAD8VR::Stereo
{
// Installs the ExecuteCommandLists hook on the game's command queue. This is the interception
// point the whole camera hunt was trying to avoid needing: the moment between the game
// recording its scene draws and the GPU consuming the camera buffer they reference. Patching
// cb12 here lands before the GPU reads it, which patching at Present never could.
void Init(IUnknown* commandQueue) noexcept;

// Hooks D3D12CreateDevice before the game ever calls it, so the device's resource and
// descriptor entry points are instrumented from its first allocation. Installing at swapchain
// creation was two seconds too late: the engine builds its constant arenas during device init,
// and anything allocated before the hook can never be translated afterwards.
void InstallDeviceCreationHook() noexcept;

// Called from the swapchain's Present so the per-frame patch happens once per frame.
void OnPresent() noexcept;

void Shutdown() noexcept;
}
