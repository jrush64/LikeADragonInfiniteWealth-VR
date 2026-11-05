#include "dxgi_wrappers.h"

#include "d3d_capture.h"
#include "lad_stereo.h"
#include "lad_xr.h"
#include "logger.h"

#include <dxgi1_6.h>

#include <atomic>

namespace
{
template <typename T>
void SafeRelease(T*& p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// Every wrapper answers QueryInterface for its own interface chain and forwards anything
// else to the real object. Returning the real object for an unknown IID means the proxy loses sight
// of calls made through it, which is the accepted cost of not owning every interface.
class WrappedSwapChain final : public IDXGISwapChain4
{
public:
    explicit WrappedSwapChain(IDXGISwapChain1* real) : m_sc1(real)
    {
        m_sc1->AddRef();
        m_sc1->QueryInterface(IID_PPV_ARGS(&m_sc));
        m_sc1->QueryInterface(IID_PPV_ARGS(&m_sc2));
        m_sc1->QueryInterface(IID_PPV_ARGS(&m_sc3));
        m_sc1->QueryInterface(IID_PPV_ARGS(&m_sc4));
    }

    ~WrappedSwapChain()
    {
        SafeRelease(m_sc4);
        SafeRelease(m_sc3);
        SafeRelease(m_sc2);
        SafeRelease(m_sc);
        SafeRelease(m_sc1);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
            riid == __uuidof(IDXGISwapChain1) ||
            (riid == __uuidof(IDXGISwapChain2) && m_sc2 != nullptr) ||
            (riid == __uuidof(IDXGISwapChain3) && m_sc3 != nullptr) ||
            (riid == __uuidof(IDXGISwapChain4) && m_sc4 != nullptr))
        {
            AddRef();
            *out = this;
            return S_OK;
        }
        return m_sc1->QueryInterface(riid, out);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_refs;
        if (remaining == 0) delete this;
        return remaining;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    { return m_sc1->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* unknown) override
    { return m_sc1->SetPrivateDataInterface(guid, unknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    { return m_sc1->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    { return m_sc1->GetParent(riid, parent); }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override
    { return m_sc1->GetDevice(riid, device); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override
    {
        LAD8VR::D3DCapture::OnPresent();
        LAD8VR::Xr::OnPresent(m_sc1);
        return m_sc->Present(syncInterval, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override
    { return m_sc1->GetBuffer(buffer, riid, surface); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override
    { return m_sc1->SetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override
    { return m_sc1->GetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override
    { return m_sc1->GetDesc(desc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override
    {
        LAD8VR::D3DCapture::OnResizeBuffers(count, width, height, format, flags);
        LAD8VR::Xr::OnResize();
        const HRESULT hr = m_sc1->ResizeBuffers(count, width, height, format, flags);
        // The game asserts and breaks on a failed D3D call, so a bad HRESULT here shows up as
        // a crash inside the game with no explanation. Log it rather than infer it.
        LAD8VR::D3DCapture::OnResizeResult("ResizeBuffers", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* parameters) override
    { return m_sc1->ResizeTarget(parameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override
    { return m_sc1->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override
    { return m_sc1->GetFrameStatistics(stats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override
    { return m_sc1->GetLastPresentCount(count); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override
    { return m_sc1->GetDesc1(desc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override
    { return m_sc1->GetFullscreenDesc(desc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override { return m_sc1->GetHwnd(hwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override
    { return m_sc1->GetCoreWindow(riid, unk); }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) override
    {
        LAD8VR::D3DCapture::OnPresent();
        LAD8VR::Xr::OnPresent(m_sc1);
        return m_sc1->Present1(syncInterval, flags, parameters);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
    { return m_sc1->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override
    { return m_sc1->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override
    { return m_sc1->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override
    { return m_sc1->GetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override
    { return m_sc1->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override
    { return m_sc1->GetRotation(rotation); }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override
    { return m_sc2 != nullptr ? m_sc2->SetSourceSize(width, height) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override
    { return m_sc2 != nullptr ? m_sc2->GetSourceSize(width, height) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override
    { return m_sc2 != nullptr ? m_sc2->SetMaximumFrameLatency(latency) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override
    { return m_sc2 != nullptr ? m_sc2->GetMaximumFrameLatency(latency) : E_NOINTERFACE; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    { return m_sc2 != nullptr ? m_sc2->GetFrameLatencyWaitableObject() : nullptr; }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override
    { return m_sc2 != nullptr ? m_sc2->SetMatrixTransform(matrix) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override
    { return m_sc2 != nullptr ? m_sc2->GetMatrixTransform(matrix) : E_NOINTERFACE; }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
    { return m_sc3 != nullptr ? m_sc3->GetCurrentBackBufferIndex() : 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support) override
    { return m_sc3 != nullptr ? m_sc3->CheckColorSpaceSupport(space, support) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override
    {
        LAD8VR::D3DCapture::OnSetColorSpace(static_cast<int>(space));
        return m_sc3 != nullptr ? m_sc3->SetColorSpace1(space) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                             UINT flags, const UINT* nodeMask,
                                             IUnknown* const* queues) override
    {
        LAD8VR::D3DCapture::OnResizeBuffers(count, width, height, format, flags);
        LAD8VR::Xr::OnResize();
        if (m_sc3 == nullptr) return E_NOINTERFACE;
        const HRESULT hr = m_sc3->ResizeBuffers1(count, width, height, format, flags, nodeMask, queues);
        LAD8VR::D3DCapture::OnResizeResult("ResizeBuffers1", hr);
        return hr;
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) override
    { return m_sc4 != nullptr ? m_sc4->SetHDRMetaData(type, size, data) : E_NOINTERFACE; }

private:
    std::atomic<ULONG> m_refs{1};
    IDXGISwapChain* m_sc = nullptr;
    IDXGISwapChain1* m_sc1 = nullptr;
    IDXGISwapChain2* m_sc2 = nullptr;
    IDXGISwapChain3* m_sc3 = nullptr;
    IDXGISwapChain4* m_sc4 = nullptr;
};

class WrappedFactory final : public IDXGIFactory7
{
public:
    explicit WrappedFactory(IDXGIFactory* real) : m_f(real)
    {
        m_f->AddRef();
        m_f->QueryInterface(IID_PPV_ARGS(&m_f1));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f2));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f3));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f4));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f5));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f6));
        m_f->QueryInterface(IID_PPV_ARGS(&m_f7));
    }

    ~WrappedFactory()
    {
        SafeRelease(m_f7);
        SafeRelease(m_f6);
        SafeRelease(m_f5);
        SafeRelease(m_f4);
        SafeRelease(m_f3);
        SafeRelease(m_f2);
        SafeRelease(m_f1);
        SafeRelease(m_f);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) ||
            (riid == __uuidof(IDXGIFactory1) && m_f1 != nullptr) ||
            (riid == __uuidof(IDXGIFactory2) && m_f2 != nullptr) ||
            (riid == __uuidof(IDXGIFactory3) && m_f3 != nullptr) ||
            (riid == __uuidof(IDXGIFactory4) && m_f4 != nullptr) ||
            (riid == __uuidof(IDXGIFactory5) && m_f5 != nullptr) ||
            (riid == __uuidof(IDXGIFactory6) && m_f6 != nullptr) ||
            (riid == __uuidof(IDXGIFactory7) && m_f7 != nullptr))
        {
            AddRef();
            *out = this;
            return S_OK;
        }
        return m_f->QueryInterface(riid, out);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_refs;
        if (remaining == 0) delete this;
        return remaining;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    { return m_f->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* unknown) override
    { return m_f->SetPrivateDataInterface(guid, unknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    { return m_f->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    { return m_f->GetParent(riid, parent); }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override
    { return m_f->EnumAdapters(index, adapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND hwnd, UINT flags) override
    { return m_f->MakeWindowAssociation(hwnd, flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* hwnd) override
    { return m_f->GetWindowAssociation(hwnd); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) override
    {
        // The game uses CreateSwapChainForHwnd. This path is left unwrapped on purpose: if it
        // ever fires, the log line says so rather than silently going unhooked.
        LAD8VR::Log::Line("[SWAPCHAIN] legacy CreateSwapChain used, NOT wrapped");
        return m_f->CreateSwapChain(device, desc, out);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override
    { return m_f->CreateSoftwareAdapter(module, adapter); }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override
    { return m_f1 != nullptr ? m_f1->EnumAdapters1(index, adapter) : E_NOINTERFACE; }
    BOOL STDMETHODCALLTYPE IsCurrent() override
    { return m_f1 != nullptr ? m_f1->IsCurrent() : FALSE; }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override
    { return m_f2 != nullptr ? m_f2->IsWindowedStereoEnabled() : FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                     IDXGIOutput* restrictTo,
                                                     IDXGISwapChain1** out) override
    {
        if (m_f2 == nullptr) return E_NOINTERFACE;
        const HRESULT hr = m_f2->CreateSwapChainForHwnd(device, hwnd, desc, fullscreen, restrictTo, out);
        if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
        {
            LAD8VR::D3DCapture::OnSwapChainCreated(device, *out);
            // The device handed to CreateSwapChainForHwnd IS the command queue on D3D12,
            // which is exactly the object whose ExecuteCommandLists the proxy needs.
            LAD8VR::Stereo::Init(device);
            LAD8VR::Xr::OnSwapChainReady(device, *out);
            IDXGISwapChain1* real = *out;
            *out = new WrappedSwapChain(real);
            real->Release();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* restrictTo,
                                                           IDXGISwapChain1** out) override
    { return m_f2 != nullptr ? m_f2->CreateSwapChainForCoreWindow(device, window, desc, restrictTo, out)
                             : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override
    { return m_f2 != nullptr ? m_f2->GetSharedResourceAdapterLuid(resource, luid) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND hwnd, UINT message, DWORD* cookie) override
    { return m_f2 != nullptr ? m_f2->RegisterStereoStatusWindow(hwnd, message, cookie) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override
    { return m_f2 != nullptr ? m_f2->RegisterStereoStatusEvent(event, cookie) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override
    { if (m_f2 != nullptr) m_f2->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND hwnd, UINT message, DWORD* cookie) override
    { return m_f2 != nullptr ? m_f2->RegisterOcclusionStatusWindow(hwnd, message, cookie) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override
    { return m_f2 != nullptr ? m_f2->RegisterOcclusionStatusEvent(event, cookie) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override
    { if (m_f2 != nullptr) m_f2->UnregisterOcclusionStatus(cookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* restrictTo,
                                                            IDXGISwapChain1** out) override
    { return m_f2 != nullptr ? m_f2->CreateSwapChainForComposition(device, desc, restrictTo, out)
                             : E_NOINTERFACE; }

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override
    { return m_f3 != nullptr ? m_f3->GetCreationFlags() : 0; }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) override
    { return m_f4 != nullptr ? m_f4->EnumAdapterByLuid(luid, riid, adapter) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override
    { return m_f4 != nullptr ? m_f4->EnumWarpAdapter(riid, adapter) : E_NOINTERFACE; }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override
    { return m_f5 != nullptr ? m_f5->CheckFeatureSupport(feature, data, size) : E_NOINTERFACE; }

    // IDXGIFactory6
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference,
                                                         REFIID riid, void** adapter) override
    { return m_f6 != nullptr ? m_f6->EnumAdapterByGpuPreference(index, preference, riid, adapter)
                             : E_NOINTERFACE; }

    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override
    { return m_f7 != nullptr ? m_f7->RegisterAdaptersChangedEvent(event, cookie) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override
    { return m_f7 != nullptr ? m_f7->UnregisterAdaptersChangedEvent(cookie) : E_NOINTERFACE; }

private:
    std::atomic<ULONG> m_refs{1};
    IDXGIFactory* m_f = nullptr;
    IDXGIFactory1* m_f1 = nullptr;
    IDXGIFactory2* m_f2 = nullptr;
    IDXGIFactory3* m_f3 = nullptr;
    IDXGIFactory4* m_f4 = nullptr;
    IDXGIFactory5* m_f5 = nullptr;
    IDXGIFactory6* m_f6 = nullptr;
    IDXGIFactory7* m_f7 = nullptr;
};
}

namespace LAD8VR
{
IUnknown* WrapFactory(IUnknown* real) noexcept
{
    if (real == nullptr) return nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(real->QueryInterface(IID_PPV_ARGS(&factory))) || factory == nullptr)
    {
        Log::Line("[HOOK] object is not IDXGIFactory, left unwrapped");
        return nullptr;
    }
    auto* wrapper = new WrappedFactory(factory);
    factory->Release();
    return static_cast<IDXGIFactory7*>(wrapper);
}
}
