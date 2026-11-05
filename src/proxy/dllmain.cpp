#include "d3d_capture.h"
#include "lad_engine_aer.h"
#include "lad_stereo.h"
#include "lad_xr.h"
#include "logger.h"

#include <windows.h>

#include <iterator>
#include <string>

namespace
{
void LogProcessIdentity()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    LAD8VR::Log::Line("[BOOT] LAD8VR passive dxgi probe attached");
    LAD8VR::Log::Line("[BOOT] host process: " + LAD8VR::Log::WideToUtf8(path));

    char line[128] = {};
    sprintf_s(line, "[BOOT] pid=%lu", GetCurrentProcessId());
    LAD8VR::Log::Line(line);
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        LAD8VR::Log::Open();
        LogProcessIdentity();
        // Must run before the game creates its D3D12 device, which is why it is here and not
        // at swapchain creation. LoadLibrary of d3d12.dll from DllMain is safe: it is already
        // in the game's static import table, so it is loaded either way.
        LAD8VR::Stereo::InstallDeviceCreationHook();
        break;
    case DLL_PROCESS_DETACH:
        // Never wait on anything from here. A join inside DllMain deadlocks against the
        // loader lock and hangs the process on exit.
        LAD8VR::D3DCapture::Shutdown();
        LAD8VR::EngineAer::Shutdown();
        LAD8VR::Xr::Shutdown();
        LAD8VR::Log::Close();
        break;
    default:
        break;
    }
    return TRUE;
}
