@echo off
REM Build the RESEARCH dxgi.dll proxy (x64, static CRT). Not part of the release; see
REM docs/TECHNICAL_NOTES.md ("The research dxgi.dll proxy") before using it.
REM Output: builds\research\dxgi.dll (and dxgi.map)
REM Prerequisites: Visual Studio 2022 with the "Desktop development with C++" workload.
REM At runtime the DLL imports openxr_loader.dll (Khronos OpenXR loader, x64), which is not
REM built or shipped by this repo.
setlocal
cd /d "%~dp0"

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [build] no VS install with the C++ toolset found & exit /b 1 )

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

for %%I in ("%~dp0..") do set ROOT=%%~fI
set SRC=..\src\proxy
set MH=..\third_party\minhook
if not exist "%MH%\src\hook.c" ( echo [build] minhook not found at "%MH%" & exit /b 1 )
set XR=..\third_party\openxr
if not exist "%XR%\include\openxr\openxr.h" ( echo [build] OpenXR headers not found at "%XR%" & exit /b 1 )

set OUTDIR=%~dp0..\builds\research
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set OBJDIR=%OUTDIR%\obj
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM Import library for openxr_loader.dll, generated from a .def so no prebuilt .lib is vendored.
lib /nologo /machine:x64 /def:"%XR%\openxr_loader.def" /out:"%OBJDIR%\openxr_loader.lib"
if errorlevel 1 ( echo [build] openxr import library FAILED & exit /b 1 )

REM NO dxgi.lib on the link line: a DLL named dxgi.dll that statically imports dxgi.dll is
REM self-recursive. The real exports are resolved with LoadLibrary/GetProcAddress instead.
REM dxguid.lib supplies the interface IIDs used by QueryInterface.
cl /nologo /O2 /MT /LD /EHsc /std:c++17 "/d1trimfile:%ROOT%\\" ^
  /I"%XR%\include" /I"%MH%\include" /I"%MH%\src\hde" /Fo"%OBJDIR%\\" ^
  "%SRC%\dxgi_proxy.cpp" "%SRC%\dxgi_wrappers.cpp" "%SRC%\d3d_capture.cpp" "%SRC%\lad_xr.cpp" ^
  "%SRC%\lad_camera.cpp" "%SRC%\lad_cam_trace.cpp" "%SRC%\lad_engine_aer.cpp" "%SRC%\lad_stereo.cpp" ^
  "%SRC%\dllmain.cpp" "%SRC%\logger.cpp" ^
  "%MH%\src\hde\hde64.c" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" ^
  /Fe:"%OUTDIR%\dxgi.dll" ^
  /link "%OBJDIR%\openxr_loader.lib" user32.lib ole32.lib shell32.lib dxguid.lib advapi32.lib ^
  /DEF:"%SRC%\dxgi_proxy.def" /MAP:"%OUTDIR%\dxgi.map"
if errorlevel 1 ( echo [build] compile/link FAILED & exit /b 1 )

del /q "%OUTDIR%\dxgi.exp" "%OUTDIR%\dxgi.lib" 2>nul
rmdir /s /q "%OBJDIR%" 2>nul

echo [build] OK -^> "%OUTDIR%\dxgi.dll"
endlocal
