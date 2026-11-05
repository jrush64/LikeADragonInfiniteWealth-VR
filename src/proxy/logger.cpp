#include "logger.h"

#include <shlobj.h>

#include <cstdio>
#include <mutex>
#include <share.h>

namespace
{
FILE* g_file = nullptr;
std::mutex g_lock;
bool g_opened = false;

std::wstring LogDirectory()
{
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local != nullptr)
    {
        dir.assign(local);
        CoTaskMemFree(local);
    }
    if (dir.empty()) dir = L"C:\\Temp";
    dir += L"\\LAD8VR";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteLocked(const std::string& text)
{
    if (g_file == nullptr) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    fprintf(g_file, "%02u:%02u:%02u.%03u [%lu] %s\n", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, GetCurrentThreadId(), text.c_str());
    fflush(g_file);
}
}

namespace LAD8VR::Log
{
void Open() noexcept
{
    try
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_opened) return;
        g_opened = true;
        const std::wstring path = LogDirectory() + L"\\LAD8VR_Log.txt";
        // _SH_DENYWR so the log can be read while the game is still running. Reading a live
        // log is the only way to diagnose a hang, and an exclusive handle blocks that.
        g_file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
        WriteLocked("========================================");
    }
    catch (...)
    {
    }
}

void Close() noexcept
{
    try
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_file != nullptr)
        {
            WriteLocked("[LOG] closing");
            fclose(g_file);
            g_file = nullptr;
        }
    }
    catch (...)
    {
    }
}

void Line(const std::string& text) noexcept
{
    try
    {
        std::lock_guard<std::mutex> guard(g_lock);
        WriteLocked(text);
    }
    catch (...)
    {
    }
}

void WindowsError(const char* what, DWORD error) noexcept
{
    char buffer[256] = {};
    sprintf_s(buffer, "%s failed, GetLastError=%lu", what != nullptr ? what : "<null>", error);
    Line(buffer);
}

std::string WideToUtf8(const std::wstring& text) noexcept
{
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}
}
