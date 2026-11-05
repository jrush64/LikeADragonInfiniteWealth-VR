#include "dxgi_proxy.h"

#include "dxgi_wrappers.h"
#include "logger.h"

#include <iterator>
#include <mutex>
#include <string>

namespace
{
// Replaces the real factory the caller was about to receive with the proxy's wrapper. The caller's
// reference to the real object is handed to the wrapper, so the net refcount is unchanged.
void SwapInWrapper(void** factory, const char* origin) noexcept
{
    if (factory == nullptr || *factory == nullptr) return;
    IUnknown* real = static_cast<IUnknown*>(*factory);
    IUnknown* wrapper = LAD8VR::WrapFactory(real);
    if (wrapper == nullptr) return;
    real->Release();
    *factory = wrapper;
    LAD8VR::Log::Line(std::string("[HOOK] wrapped factory from ") + origin);
}
}

namespace
{
HMODULE g_realDxgi = nullptr;
HRESULT g_loadResult = E_FAIL;
std::once_flag g_loadOnce;

std::wstring ModuleDirectory()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
    wchar_t path[MAX_PATH] = {};
    if (self == nullptr || GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path))) == 0)
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash + 1);
}

std::wstring SystemDxgiPath()
{
    wchar_t systemDir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir)));
    if (length == 0 || length >= static_cast<UINT>(std::size(systemDir)))
        return L"C:\\Windows\\System32\\dxgi.dll";
    std::wstring path(systemDir);
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += L"dxgi.dll";
    return path;
}

void LoadRealDxgi() noexcept
{
    // The game folder already had a ReShade dxgi.dll. Rather than displace it, chain to it if
    // it has been renamed to dxgi_chain.dll, so both loads survive.
    const std::wstring chain = ModuleDirectory() + L"dxgi_chain.dll";
    if (!chain.empty() && GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        g_realDxgi = LoadLibraryW(chain.c_str());
        if (g_realDxgi != nullptr)
        {
            g_loadResult = S_OK;
            LAD8VR::Log::Line("[DXGI] chaining to " + LAD8VR::Log::WideToUtf8(chain));
            return;
        }
        LAD8VR::Log::WindowsError("[DXGI] LoadLibraryW dxgi_chain.dll", GetLastError());
    }

    const std::wstring path = SystemDxgiPath();
    g_realDxgi = LoadLibraryW(path.c_str());
    if (g_realDxgi == nullptr)
    {
        const DWORD error = GetLastError();
        g_loadResult = HRESULT_FROM_WIN32(error);
        LAD8VR::Log::WindowsError("[DXGI] LoadLibraryW real dxgi.dll", error);
        return;
    }
    g_loadResult = S_OK;
    LAD8VR::Log::Line("[DXGI] loaded real dxgi.dll: " + LAD8VR::Log::WideToUtf8(path));
}

template <typename Fn>
Fn ResolveTyped(const char* name) noexcept
{
    return reinterpret_cast<Fn>(LAD8VR::Dxgi::Resolve(name));
}
}

namespace LAD8VR::Dxgi
{
bool EnsureLoaded() noexcept
{
    try
    {
        std::call_once(g_loadOnce, LoadRealDxgi);
        return SUCCEEDED(g_loadResult) && g_realDxgi != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

FARPROC Resolve(const char* name) noexcept
{
    if (name == nullptr || name[0] == '\0') return nullptr;
    if (!EnsureLoaded())
    {
        Log::Line(std::string("[DXGI] cannot resolve export, real dxgi not loaded: ") + name);
        return nullptr;
    }
    FARPROC proc = GetProcAddress(g_realDxgi, name);
    if (proc == nullptr)
        Log::WindowsError((std::string("[DXGI] missing real dxgi export: ") + name).c_str(), GetLastError());
    return proc;
}

HRESULT MissingExport(const char* name) noexcept
{
    Log::Line(std::string("[DXGI] returning failure for missing export: ") +
              (name != nullptr ? name : "<null>"));
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("CreateDXGIFactory");
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr)) SwapInWrapper(factory, "CreateDXGIFactory");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory1");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("CreateDXGIFactory1");
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr)) SwapInWrapper(factory, "CreateDXGIFactory1");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory2");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("CreateDXGIFactory2");
    const HRESULT hr = fn(flags, riid, factory);
    if (SUCCEEDED(hr)) SwapInWrapper(factory, "CreateDXGIFactory2");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    const auto fn = ResolveTyped<Fn>("DXGIDeclareAdapterRemovalSupport");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("DXGIDeclareAdapterRemovalSupport");
    return fn();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface(REFIID riid, void** debugInterface)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("DXGIGetDebugInterface");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("DXGIGetDebugInterface");
    return fn(riid, debugInterface);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto fn = ResolveTyped<Fn>("DXGIGetDebugInterface1");
    if (fn == nullptr) return LAD8VR::Dxgi::MissingExport("DXGIGetDebugInterface1");
    return fn(flags, riid, debugInterface);
}
