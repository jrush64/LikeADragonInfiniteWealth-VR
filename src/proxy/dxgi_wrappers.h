#pragma once

#include <windows.h>

// Deliberately no dxgi headers here. This header is included by the translation unit that
// *defines* the dxgi exports, and the SDK's own declarations of CreateDXGIFactory and friends
// clash with this module's dllexport definitions (C2375, different linkage).
struct IDXGIFactory;

namespace LAD8VR
{
// Wraps a real DXGI factory so CreateSwapChainForHwnd can hand back a wrapped swapchain.
// Returns nullptr if the object is not a factory, in which case the caller keeps the real one.
//
// This exists instead of vtable patching. The Steam overlay patches the shared swapchain
// vtable and re-patches on every swapchain creation, so a second patcher gets captured as
// the overlay's "original" and the two call each other until the stack overflows (this was
// observed on this game). A wrapper never writes into a vtable, so nothing can capture it, and
// the overlay's own hooks still run underneath the wrapper as normal.
IUnknown* WrapFactory(IUnknown* real) noexcept;
}
