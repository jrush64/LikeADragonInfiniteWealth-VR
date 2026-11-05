// Passive D3D12 probe for Like a Dragon: Infinite Wealth.
//
// Nothing here changes what the game renders. The interception lives in dxgi_wrappers.cpp;
// this file only records what the renderer actually is: swapchain shape and format, colour
// space, present queue, device nodes and the view-instancing tier.

#include "d3d_capture.h"

#include "lad_camera.h"
#include "lad_engine_aer.h"
#include "lad_stereo.h"
#include "logger.h"

#include <d3d12.h>

#include <string>

namespace
{
ULONGLONG g_lastReport = 0;
unsigned long long g_presents = 0;
unsigned long long g_presentsAtReport = 0;
bool g_colorSpaceSeen = false;

const char* FormatName(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

const char* ColorSpaceName(int space) noexcept
{
    switch (space)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709 (sRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709 (scRGB linear)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020 (HDR10 PQ)";
    default: return "other";
    }
}

// The pDevice passed to CreateSwapChainForHwnd is a command queue on D3D12, not a device.
// Walking queue -> device is what reveals the queue topology and the feature tiers that
// decide whether view instancing is even on the table for stereo.
void DescribeDevice(IUnknown* deviceOrQueue) noexcept
{
    if (deviceOrQueue == nullptr) return;

    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
    {
        LAD8VR::Log::Line("[D3D] swapchain device is NOT an ID3D12CommandQueue");
        return;
    }

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    char line[256] = {};
    sprintf_s(line, "[D3D] present queue type=%d priority=%d flags=0x%X nodeMask=0x%X",
              static_cast<int>(desc.Type), desc.Priority, static_cast<unsigned>(desc.Flags),
              desc.NodeMask);
    LAD8VR::Log::Line(line);

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
    {
        sprintf_s(line, "[D3D] device nodeCount=%u", device->GetNodeCount());
        LAD8VR::Log::Line(line);

        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))))
        {
            // Tier 1+ means the hardware can render both eyes in one pass via SV_ViewID. The
            // shaders as shipped do not use it (UsesViewID is false everywhere), so this only
            // says whether that door could be opened, not that it is open.
            sprintf_s(line, "[D3D] ViewInstancingTier=%d", static_cast<int>(options3.ViewInstancingTier));
            LAD8VR::Log::Line(line);
        }
        device->Release();
    }
    queue->Release();
}

void DescribeSwapChain(IDXGISwapChain1* swapchain) noexcept
{
    if (swapchain == nullptr) return;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(swapchain->GetDesc1(&desc))) return;
    char line[320] = {};
    sprintf_s(line,
              "[SWAPCHAIN] %ux%u format=%s(%d) buffers=%u swapEffect=%d alphaMode=%d "
              "flags=0x%X sampleCount=%u stereo=%d scaling=%d",
              desc.Width, desc.Height, FormatName(desc.Format), static_cast<int>(desc.Format),
              desc.BufferCount, static_cast<int>(desc.SwapEffect), static_cast<int>(desc.AlphaMode),
              desc.Flags, desc.SampleDesc.Count, desc.Stereo ? 1 : 0, static_cast<int>(desc.Scaling));
    LAD8VR::Log::Line(line);
}
}

namespace LAD8VR::D3DCapture
{
void OnSwapChainCreated(IUnknown* deviceOrQueue, IDXGISwapChain1* swapchain) noexcept
{
    try
    {
        Log::Line("[SWAPCHAIN] created and wrapped");
        DescribeDevice(deviceOrQueue);
        DescribeSwapChain(swapchain);
    }
    catch (...)
    {
    }
}

void OnPresent() noexcept
{
    ++g_presents;

    // The camera hunt lives here rather than on the XR present path on purpose: it is pure
    // memory work and has nothing to do with the headset, so it must run on a flat launch
    // too. Hanging it off the XR path meant it only ran with a session live, which forced a
    // headset on for what is really a debugger task.
    Camera::Start();
    Camera::Tick();
    EngineAer::Start();
    EngineAer::Tick();
    Stereo::OnPresent();

    const ULONGLONG now = GetTickCount64();
    if (g_lastReport == 0)
    {
        g_lastReport = now;
        g_presentsAtReport = g_presents;
        Log::Line("[FRAME] first present");
        if (!g_colorSpaceSeen)
            Log::Line("[SWAPCHAIN] no SetColorSpace1 before the first present, treating as SDR");
        return;
    }
    if (now - g_lastReport < 3000) return;
    const unsigned long long delta = g_presents - g_presentsAtReport;
    char line[160] = {};
    sprintf_s(line, "[FRAME] presents=%llu (+%llu in %llums)", g_presents, delta, now - g_lastReport);
    Log::Line(line);
    g_lastReport = now;
    g_presentsAtReport = g_presents;
}

void OnResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) noexcept
{
    char line[200] = {};
    sprintf_s(line, "[SWAPCHAIN] ResizeBuffers %ux%u count=%u format=%s(%d) flags=0x%X", width, height,
              bufferCount, FormatName(format), static_cast<int>(format), flags);
    Log::Line(line);
}

void OnResizeResult(const char* what, HRESULT hr) noexcept
{
    const char* meaning = "";
    switch (static_cast<unsigned>(hr))
    {
    case 0x887A0001: meaning = " DXGI_ERROR_INVALID_CALL (outstanding references to the buffers)"; break;
    case 0x887A0005: meaning = " DXGI_ERROR_DEVICE_REMOVED"; break;
    case 0x887A0006: meaning = " DXGI_ERROR_DEVICE_HUNG"; break;
    case 0x887A0007: meaning = " DXGI_ERROR_DEVICE_RESET"; break;
    case 0x887A0020: meaning = " DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
    case 0x80070057: meaning = " E_INVALIDARG"; break;
    case 0x8007000E: meaning = " E_OUTOFMEMORY"; break;
    default: break;
    }
    char line[220] = {};
    sprintf_s(line, "[SWAPCHAIN] %s returned 0x%08X%s", what, static_cast<unsigned>(hr), meaning);
    Log::Line(line);
}

void OnSetColorSpace(int colorSpace) noexcept
{
    g_colorSpaceSeen = true;
    Log::Line(std::string("[SWAPCHAIN] SetColorSpace1 -> ") + ColorSpaceName(colorSpace));
}

void Shutdown() noexcept
{
    char line[128] = {};
    sprintf_s(line, "[FRAME] total presents=%llu", g_presents);
    Log::Line(line);
}
}
