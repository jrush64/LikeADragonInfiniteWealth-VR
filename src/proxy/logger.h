#pragma once

#include <windows.h>

#include <string>

namespace LAD8VR::Log
{
// Opens %LOCALAPPDATA%\LAD8VR\LAD8VR_Log.txt. Safe to call more than once.
void Open() noexcept;
void Close() noexcept;

void Line(const std::string& text) noexcept;
void WindowsError(const char* what, DWORD error) noexcept;

std::string WideToUtf8(const std::wstring& text) noexcept;
}
