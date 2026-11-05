@echo off
REM Build the Like a Dragon: Infinite Wealth VR ReShade add-on (x64, static CRT).
REM   scripts\build.bat        -> builds\LAD8VR.addon64 (and builds\LAD8VR.map)
REM   scripts\build.bat v41    -> builds\experimental\LAD8VR_v41_untested.addon64
REM Prerequisites: Visual Studio 2022 (Community/Professional/Enterprise or the standalone
REM Build Tools) with the "Desktop development with C++" workload. See BUILD.md.
setlocal
cd /d "%~dp0"

REM Locate any VS2022 install via vswhere instead of assuming one edition or drive letter.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [build] no VS install with the C++ toolset found & exit /b 1 )

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

for %%I in ("%~dp0..") do set ROOT=%%~fI
set SRC=..\src\addon
set RESHADE=..\third_party\reshade\include
if not exist "%RESHADE%\reshade\reshade.hpp" ( echo [build] ReShade add-on headers not found at "%RESHADE%" & exit /b 1 )

if /i "%~1"=="v41" (
  set OUTDIR=%~dp0..\builds\experimental
  set SOURCE=%SRC%\experimental\lad8vr_ui_overlay_v41.cpp
  set OUTNAME=LAD8VR_v41_untested
) else (
  set OUTDIR=%~dp0..\builds
  set SOURCE=%SRC%\lad8vr_stereo.cpp
  set OUTNAME=LAD8VR
)
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set OBJDIR=%OUTDIR%\obj
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM /MT  = static CRT, so no VC++ runtime is needed beside the add-on.
REM /EHsc matches the original builds; the __try guards sit in functions without C++ objects.
REM NAME and DESCRIPTION are dllexported in the source for ReShade's add-on list.
REM /d1trimfile strips the repo folder from __FILE__ paths so no local path ends up in the binary.
cl /nologo /std:c++20 /O2 /MT /EHsc /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  "/d1trimfile:%ROOT%\\" /I"%RESHADE%" /Fo"%OBJDIR%\\" "%SOURCE%" ^
  /Fe:"%OUTDIR%\%OUTNAME%.addon64" ^
  /link user32.lib /MAP:"%OUTDIR%\%OUTNAME%.map"
if errorlevel 1 ( echo [build] compile/link FAILED & exit /b 1 )

REM Drop linker by-products nobody deploys.
del /q "%OUTDIR%\%OUTNAME%.exp" "%OUTDIR%\%OUTNAME%.lib" 2>nul
rmdir /s /q "%OBJDIR%" 2>nul

echo [build] OK -^> "%OUTDIR%\%OUTNAME%.addon64"
endlocal
