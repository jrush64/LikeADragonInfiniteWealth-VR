// OpenXR output for the research dxgi.dll proxy (D3D12).
//
// RESEARCH CODE. This file began as a mono, head-tracked virtual screen (the game's backbuffer
// copied into an OpenXR image and shown as a large screen in front of the viewer). It was then
// changed into the "AER" experiment: one OpenXR swapchain per eye, each refreshed only on the
// frames that lad_engine_aer.cpp tagged with that eye, submitted as two eye-specific quad
// layers. The AER experiment did not produce coherent stereo (see docs/TECHNICAL_NOTES.md), so
// this file is NOT a working headset path in its current form. It is kept because the D3D12
// OpenXR bring-up (instance, session, sRGB swapchain choice, blit pipeline, resize and
// shutdown handling) is reusable for delivering the add-on's native stereo to a headset.
//
// Everything runs inline on the present thread. A separate submit worker was avoided on
// purpose: in an earlier project a worker thread silently re-rated per-frame counters, and the
// game's ~120 fps leaves no cadence problem for inline submission to solve.

#include "lad_xr.h"

#include "lad_camera.h"
#include "lad_engine_aer.h"
#include "logger.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

namespace
{
constexpr uint32_t kEyeCount = 2;
constexpr uint32_t kFrameSlots = 3;

struct Eye
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
};

bool g_failed = false;
bool g_ready = false;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_systemId = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_sessionRunning = false;

Eye g_eyes[kEyeCount];
bool g_eyePopulated[kEyeCount] = {};
uint32_t g_width = 0;
uint32_t g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;
DXGI_FORMAT g_eyeFormat = DXGI_FORMAT_UNKNOWN;

// Blit pipeline. The runtime will not take the game's R10G10B10A2 backbuffer (VirtualDesktopXR
// offers 8 bit and FP16 only), and D3D12 copies can neither scale nor convert, so the frame has
// to go through a shader. That was always going to be needed for stereo anyway: cropping an
// SBS pair and sliding a convergence window are both UV work in this same pixel shader.
ID3D12RootSignature* g_rootSignature = nullptr;
ID3D12PipelineState* g_pipeline = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_rtvStride = 0;
ID3D12Resource* g_sourceCopy = nullptr;
bool g_sourceInCopyDest = true;
bool g_resizePending = false;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_allocators[kFrameSlots] = {};
ID3D12GraphicsCommandList* g_lists[kFrameSlots] = {};
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValue = 0;
UINT64 g_slotFence[kFrameSlots] = {};
uint32_t g_slot = 0;

unsigned long long g_submitted = 0;
ULONGLONG g_lastReport = 0;

void Fail(const char* what, XrResult res)
{
    char line[256] = {};
    sprintf_s(line, "[XR] %s failed, XrResult=%d", what, static_cast<int>(res));
    LAD8VR::Log::Line(line);
    g_failed = true;
}

const char* FormatName(DXGI_FORMAT format)
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

bool CreateInstanceAndSystem()
{
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    strcpy_s(info.applicationInfo.applicationName, "LAD8VR");
    info.applicationInfo.applicationVersion = 1;
    strcpy_s(info.applicationInfo.engineName, "elvis");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult res = xrCreateInstance(&info, &g_instance);
    if (XR_FAILED(res))
    {
        Fail("xrCreateInstance", res);
        return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props)))
        LAD8VR::Log::Line(std::string("[XR] runtime: ") + props.runtimeName);

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    res = xrGetSystem(g_instance, &systemInfo, &g_systemId);
    if (XR_FAILED(res))
    {
        Fail("xrGetSystem", res);
        return false;
    }
    return true;
}

bool CreateSession()
{
    // The runtime must be told which adapter it is sharing, and it refuses the session if the
    // graphics requirements call is skipped, even though nothing here uses the result.
    PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    if (getRequirements == nullptr)
    {
        LAD8VR::Log::Line("[XR] xrGetD3D12GraphicsRequirementsKHR missing");
        g_failed = true;
        return false;
    }
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    getRequirements(g_instance, g_systemId, &reqs);

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = g_device;
    binding.queue = g_queue;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g_systemId;
    XrResult res = xrCreateSession(g_instance, &info, &g_session);
    if (XR_FAILED(res))
    {
        Fail("xrCreateSession", res);
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    res = xrCreateReferenceSpace(g_session, &spaceInfo, &g_space);
    if (XR_FAILED(res))
    {
        Fail("xrCreateReferenceSpace", res);
        return false;
    }
    return true;
}

bool CreateEyeSwapchains()
{
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    if (formatCount > 0)
        xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount, formats.data());

    // Prefer an sRGB eye format. The game's backbuffer holds sRGB-encoded values (confirmed
    // live: SetColorSpace1 -> RGB_FULL_G22_NONE_P709), and runtimes treat a plain UNORM
    // swapchain as linear. Handing sRGB bits to a linear swapchain is the washed-out picture.
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (DXGI_FORMAT want : preferred)
    {
        for (int64_t f : formats)
        {
            if (static_cast<DXGI_FORMAT>(f) == want)
            {
                g_eyeFormat = want;
                break;
            }
        }
        if (g_eyeFormat != DXGI_FORMAT_UNKNOWN) break;
    }
    if (g_eyeFormat == DXGI_FORMAT_UNKNOWN)
    {
        std::string list;
        for (int64_t f : formats)
            list += " " + std::to_string(f);
        LAD8VR::Log::Line("[XR] runtime offers no sRGB eye format, offered:" + list);
        g_failed = true;
        return false;
    }

    // Render at the runtime's own recommended eye size rather than the backbuffer size. The
    // blit shader scales, so there is no longer any reason to make the runtime carry 4K
    // images it is only going to resample anyway.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount >= kEyeCount &&
        XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_systemId,
                                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       viewCount, &viewCount, configViews.data())))
    {
        g_eyeWidth = configViews[0].recommendedImageRectWidth;
        g_eyeHeight = configViews[0].recommendedImageRectHeight;
    }
    if (g_eyeWidth == 0 || g_eyeHeight == 0)
    {
        g_eyeWidth = g_width;
        g_eyeHeight = g_height;
    }

    // AER retains one independently updated image per eye. Each game frame updates only the
    // swapchain matching the camera offset that rendered it; the other eye keeps its previous
    // completed image. Both are then submitted together as eye-specific quad layers.
    g_eyeHeight = static_cast<uint32_t>(
        static_cast<uint64_t>(g_eyeWidth) * g_height / (g_width != 0 ? g_width : 1));

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = static_cast<int64_t>(g_eyeFormat);
    info.sampleCount = 1;
    info.width = g_eyeWidth;
    info.height = g_eyeHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    for (uint32_t eye = 0; eye < kEyeCount; ++eye)
    {
        XrResult res = xrCreateSwapchain(g_session, &info, &g_eyes[eye].swapchain);
        if (XR_FAILED(res))
        {
            Fail("xrCreateSwapchain", res);
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(g_eyes[eye].swapchain, 0, &imageCount, nullptr);
        g_eyes[eye].images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        res = xrEnumerateSwapchainImages(
            g_eyes[eye].swapchain, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_eyes[eye].images.data()));
        if (XR_FAILED(res))
        {
            Fail("xrEnumerateSwapchainImages", res);
            return false;
        }
    }

    char line[224] = {};
    sprintf_s(line,
              "[XR] AER stereo swapchains 2x %ux%u format=%s images=%zu/%zu (backbuffer %ux%u %s)",
              g_eyeWidth, g_eyeHeight, FormatName(g_eyeFormat), g_eyes[0].images.size(),
              g_eyes[1].images.size(), g_width, g_height, FormatName(g_format));
    LAD8VR::Log::Line(line);
    return true;
}

const char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(abs(c + 0.055) / 1.055, 2.4);
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 c = src.SampleLevel(smp, i.uv, 0).rgb;
    // The backbuffer holds sRGB-encoded values in a plain UNORM format, so sampling returns
    // the encoded value with no decode. The eye image is _SRGB and re-encodes on write, so
    // the decode has to happen here or the picture comes out double-encoded and dark.
    return float4(SrgbToLinear(c), 1.0);
}
)";

bool CreateBlitPipeline()
{
    // Both entry points are resolved rather than statically imported. A DLL named dxgi.dll
    // that statically imports d3d12.dll would pull d3d12 in during this module's DllMain, and
    // d3d12 imports dxgi straight back into this proxy. Resolving late avoids that loop entirely.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (d3d12 == nullptr || compiler == nullptr)
    {
        LAD8VR::Log::Line("[XR] d3d12.dll or d3dcompiler_47.dll not available for the blit");
        return false;
    }
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                         ID3DBlob**, ID3DBlob**);
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, IUnknown*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto serialize = reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (serialize == nullptr || compile == nullptr)
    {
        LAD8VR::Log::Line("[XR] could not resolve D3D12SerializeRootSignature or D3DCompile");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &param;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rootBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    if (FAILED(serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob)) ||
        rootBlob == nullptr)
    {
        LAD8VR::Log::Line("[XR] root signature serialize failed");
        if (errorBlob != nullptr) errorBlob->Release();
        return false;
    }
    const HRESULT rootHr = g_device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                                          rootBlob->GetBufferSize(),
                                                          IID_PPV_ARGS(&g_rootSignature));
    rootBlob->Release();
    if (errorBlob != nullptr) errorBlob->Release();
    if (FAILED(rootHr)) return false;

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* compileError = nullptr;
    if (FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "VSMain",
                       "vs_5_1", 0, 0, &vs, &compileError)) ||
        FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "PSMain",
                       "ps_5_1", 0, 0, &ps, &compileError)))
    {
        LAD8VR::Log::Line("[XR] blit shader compile failed");
        if (compileError != nullptr)
        {
            LAD8VR::Log::Line(std::string("[XR] ") +
                              static_cast<const char*>(compileError->GetBufferPointer()));
            compileError->Release();
        }
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSignature;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    for (auto& target : pso.BlendState.RenderTarget)
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = g_eyeFormat;
    pso.SampleDesc.Count = 1;
    const HRESULT psoHr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipeline));
    vs->Release();
    ps->Release();
    if (FAILED(psoHr))
    {
        LAD8VR::Log::Line("[XR] blit PSO creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameSlots * kEyeCount;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    g_rtvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // A private copy of the backbuffer, because a swapchain buffer cannot be relied on to
    // allow an SRV. CopyResource into an identical texture always can.
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex = {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = g_width;
    tex.Height = g_height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = g_format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_sourceCopy))))
    {
        LAD8VR::Log::Line("[XR] could not create the private backbuffer copy");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = g_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_sourceCopy, &srv,
                                       g_srvHeap->GetCPUDescriptorHandleForHeapStart());
    g_sourceInCopyDest = true;

    LAD8VR::Log::Line("[XR] blit pipeline ready");
    return true;
}

void WaitForGpuIdle()
{
    if (g_fence == nullptr || g_fenceEvent == nullptr) return;
    const UINT64 target = ++g_fenceValue;
    g_queue->Signal(g_fence, target);
    if (g_fence->GetCompletedValue() < target)
    {
        g_fence->SetEventOnCompletion(target, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
    }
}

// The game boots its swapchain at a small size and resizes to the real one within a couple of
// frames, then resizes repeatedly during play. The private copy is sized from the backbuffer,
// and CopyResource between mismatched sizes removes the device, so it has to be rebuilt.
// This failure was observed directly: bring-up happened at 1600x900, the game went to 3840x2160 27 ms
// later, and the next copy took the device down.
bool RebuildSourceCopy(const DXGI_SWAP_CHAIN_DESC1& desc)
{
    if (desc.Width == g_width && desc.Height == g_height && desc.Format == g_format &&
        g_sourceCopy != nullptr)
        return true;

    WaitForGpuIdle();
    if (g_sourceCopy != nullptr)
    {
        g_sourceCopy->Release();
        g_sourceCopy = nullptr;
    }

    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex = {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = g_width;
    tex.Height = g_height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = g_format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_sourceCopy))))
    {
        LAD8VR::Log::Line("[XR] could not rebuild the private backbuffer copy after a resize");
        g_failed = true;
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = g_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_sourceCopy, &srv,
                                       g_srvHeap->GetCPUDescriptorHandleForHeapStart());
    g_sourceInCopyDest = true;

    char line[160] = {};
    sprintf_s(line, "[XR] private copy rebuilt at %ux%u %s", g_width, g_height, FormatName(g_format));
    LAD8VR::Log::Line(line);
    return true;
}

bool CreateCopyResources()
{
    for (uint32_t i = 0; i < kFrameSlots; ++i)
    {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_allocators[i]))))
            return false;
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[i],
                                               nullptr, IID_PPV_ARGS(&g_lists[i]))))
            return false;
        g_lists[i]->Close();
    }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_fenceEvent != nullptr;
}

void PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            g_state = changed->state;
            char line[96] = {};
            sprintf_s(line, "[XR] session state -> %d", static_cast<int>(g_state));
            LAD8VR::Log::Line(line);

            if (g_state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &begin)))
                {
                    g_sessionRunning = true;
                    LAD8VR::EngineAer::SetActive(true);
                    LAD8VR::Log::Line("[XR] session begun; AER eye alternation enabled");
                }
            }
            else if (g_state == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession(g_session);
                g_sessionRunning = false;
                LAD8VR::EngineAer::SetActive(false);
                LAD8VR::Log::Line("[XR] session ended; AER paused");
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    return barrier;
}

// One command list per frame: snapshot the backbuffer once, then draw it into the screen image.
void BlitFrame(ID3D12Resource* backBuffer, ID3D12Resource* screenImage)
{
    if (g_slotFence[g_slot] != 0 && g_fence->GetCompletedValue() < g_slotFence[g_slot])
    {
        g_fence->SetEventOnCompletion(g_slotFence[g_slot], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }

    ID3D12CommandAllocator* allocator = g_allocators[g_slot];
    ID3D12GraphicsCommandList* list = g_lists[g_slot];
    allocator->Reset();
    list->Reset(allocator, g_pipeline);

    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_sourceCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST)};
    // A freshly created private copy is already in COPY_DEST, so it must not be transitioned
    // into it. This resets on every rebuild, which is why it is not a function static.
    list->ResourceBarrier(g_sourceInCopyDest ? 1 : 2, pre);
    g_sourceInCopyDest = false;

    list->CopyResource(g_sourceCopy, backBuffer);

    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        Transition(g_sourceCopy, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, post);

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_rootSignature);
    list->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(g_eyeWidth),
                               static_cast<float>(g_eyeHeight), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(g_eyeWidth), static_cast<LONG>(g_eyeHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    // Two rules from the KHR spec. Breaking either removes the device with INVALID_CALL:
    //  1. XR runtimes allocate swapchain textures TYPELESS. A null RTV desc is illegal;
    //     the view format has to be stated explicitly.
    //  2. Colour images are handed over ALREADY in RENDER_TARGET state and must be released
    //     in that state. No barriers on them, in either direction.
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = g_eyeFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_slot * g_rtvStride);
    g_device->CreateRenderTargetView(screenImage, &rtvDesc, rtv);

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);

    list->Close();
    ID3D12CommandList* lists[] = {list};
    g_queue->ExecuteCommandLists(1, lists);
    g_slotFence[g_slot] = ++g_fenceValue;
    g_queue->Signal(g_fence, g_fenceValue);
    g_slot = (g_slot + 1) % kFrameSlots;
}

void ReportCadence()
{
    const ULONGLONG now = GetTickCount64();
    if (g_lastReport == 0)
    {
        g_lastReport = now;
        LAD8VR::Log::Line("[XR] first frame submitted to the headset");
        return;
    }
    if (now - g_lastReport < 3000) return;
    char line[128] = {};
    sprintf_s(line, "[XR] submitted=%llu", g_submitted);
    LAD8VR::Log::Line(line);
    g_lastReport = now;
}
}

namespace LAD8VR::Xr
{
bool Enabled() noexcept { return g_ready && !g_failed; }

void OnSwapChainReady(IUnknown* presentQueue, IDXGISwapChain1* swapchain) noexcept
{
    if (g_failed || g_ready || presentQueue == nullptr || swapchain == nullptr) return;

    // Gated on an ini so there is always a way back to a plain playable game without a
    // rebuild. The file ships with Enable=1; change the one character to stand XR down.
    wchar_t iniPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OnSwapChainReady), &self);
    GetModuleFileNameW(self, iniPath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(iniPath, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    if (GetPrivateProfileIntW(L"XR", L"Enable", 0, iniPath) == 0)
    {
        Log::Line("[XR] disabled by lad8vr.ini, probe stays passive");
        g_failed = true;
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(swapchain->GetDesc1(&desc))) return;

    if (FAILED(presentQueue->QueryInterface(IID_PPV_ARGS(&g_queue))) || g_queue == nullptr)
    {
        Log::Line("[XR] present device is not an ID3D12CommandQueue, XR disabled");
        g_failed = true;
        return;
    }
    if (FAILED(g_queue->GetDevice(IID_PPV_ARGS(&g_device))) || g_device == nullptr)
    {
        Log::Line("[XR] could not reach ID3D12Device from the queue, XR disabled");
        g_failed = true;
        return;
    }

    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;

    if (!CreateInstanceAndSystem()) return;
    if (!CreateSession()) return;
    if (!CreateEyeSwapchains()) return;
    if (!CreateCopyResources())
    {
        Log::Line("[XR] failed to create copy command resources");
        g_failed = true;
        return;
    }
    if (!CreateBlitPipeline())
    {
        g_failed = true;
        return;
    }

    g_ready = true;
    Log::Line("[XR] bring-up complete, waiting for the session to become ready");
}

void OnPresent(IDXGISwapChain1* swapchain) noexcept
{
    if (!Enabled() || swapchain == nullptr) return;

    PollEvents();
    if (!g_sessionRunning)
    {
        EngineAer::SetActive(false);
        return;
    }

    if (g_resizePending)
    {
        g_resizePending = false;
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        if (SUCCEEDED(swapchain->GetDesc1(&desc)) && !RebuildSourceCopy(desc)) return;
    }

    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(g_session, &waitInfo, &frameState))) return;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &beginInfo))) return;

    XrCompositionLayerQuad eyeLayers[kEyeCount] = {
        {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    const XrCompositionLayerBaseHeader* layerHeaders[kEyeCount] = {};
    uint32_t layerCount = 0;

    if (frameState.shouldRender)
    {
        EngineAer::SetActive(true);
        // Consume the eye tag emitted by the completed engine scene build for this frame.
        uint32_t backBufferIndex = 0;
        IDXGISwapChain3* swapchain3 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) && swapchain3 != nullptr)
        {
            backBufferIndex = swapchain3->GetCurrentBackBufferIndex();
            swapchain3->Release();
        }
        ID3D12Resource* backBuffer = nullptr;
        const bool haveBackBuffer =
            SUCCEEDED(swapchain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer))) &&
            backBuffer != nullptr;

        uint32_t eye = 0;
        if (haveBackBuffer && EngineAer::TryConsumeCompletedEye(eye))
        {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_eyes[eye].swapchain, &acquire, &index)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(g_eyes[eye].swapchain, &wait)))
            {
                BlitFrame(backBuffer, g_eyes[eye].images[index].texture);
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if (XR_SUCCEEDED(xrReleaseSwapchainImage(g_eyes[eye].swapchain, &release)))
                {
                    g_eyePopulated[eye] = true;
                }
            }
        }
        if (backBuffer != nullptr) backBuffer->Release();

        if (g_eyePopulated[0] && g_eyePopulated[1])
        {
            for (uint32_t eye = 0; eye < kEyeCount; ++eye)
            {
                XrCompositionLayerQuad& layer = eyeLayers[eye];
                layer.layerFlags = 0;
                layer.space = g_space;
                layer.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                layer.subImage.swapchain = g_eyes[eye].swapchain;
                layer.subImage.imageRect.offset = {0, 0};
                layer.subImage.imageRect.extent = {static_cast<int32_t>(g_eyeWidth),
                                                   static_cast<int32_t>(g_eyeHeight)};
                layer.subImage.imageArrayIndex = 0;
                layer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                layer.pose.position = {0.0f, 0.0f, -2.0f};
                layer.size.width = 2.4f;
                layer.size.height = 2.4f * static_cast<float>(g_eyeHeight) /
                                    static_cast<float>(g_eyeWidth != 0 ? g_eyeWidth : 1);
                layerHeaders[eye] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            }
            layerCount = kEyeCount;
            ++g_submitted;
            ReportCadence();
        }
    }
    else
    {
        EngineAer::SetActive(false);
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount > 0 ? layerHeaders : nullptr;
    xrEndFrame(g_session, &endInfo);
}
void OnResize() noexcept
{
    // Fires before the real ResizeBuffers runs, so the new size is only readable on the next
    // present. The eye swapchains are unaffected: they are sized from the runtime's
    // recommendation, not from the game, and the blit scales between the two.
    if (!g_ready) return;
    g_resizePending = true;

    // The GPU must not still be reading the buffers DXGI is about to destroy. The blit's
    // command list references the backbuffer, and nothing else waits for it.
    WaitForGpuIdle();

    // If the device is already gone, say so here: the game asserts on the failed call that
    // follows, and from inside the game that looks like an unexplained crash.
    const HRESULT removed = g_device != nullptr ? g_device->GetDeviceRemovedReason() : S_OK;
    if (FAILED(removed))
    {
        char line[160] = {};
        sprintf_s(line, "[XR] device already removed before resize, reason 0x%08X",
                  static_cast<unsigned>(removed));
        Log::Line(line);
        g_failed = true;
        return;
    }
    Log::Line("[XR] swapchain resize seen, gpu idled, private copy rebuilt next present");
}

void Shutdown() noexcept
{
    Camera::Stop();
    // Only release what cannot deadlock at process teardown. The XR session is deliberately
    // left alone: tearing it down from DllMain can hang the process on exit.
    if (g_fenceEvent != nullptr)
    {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    char line[128] = {};
    sprintf_s(line, "[XR] total frames submitted=%llu", g_submitted);
    Log::Line(line);
}
}
