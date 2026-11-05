#pragma once

#include <windows.h>

namespace LAD8VR::Dxgi
{
// Loads the DLL this proxy forwards to. Prefers dxgi_chain.dll next to this DLL (so an existing
// ReShade or other dxgi proxy keeps working) and falls back to the system dxgi.dll.
bool EnsureLoaded() noexcept;

FARPROC Resolve(const char* name) noexcept;
HRESULT MissingExport(const char* name) noexcept;
}
