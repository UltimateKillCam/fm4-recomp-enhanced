@echo off
rem Discover an installed x64 C++ toolchain. Modified 2026-10-18, GPL-3.0-only.
if /I "%VSCMD_ARG_TGT_ARCH%"=="x64" if defined LIB (
  where cl.exe >nul 2>nul
  if not errorlevel 1 exit /b 0
)
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" goto fm4_msvc_existing
set "FM4_VSWHERE="
set "FM4_VS_ROOT="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "FM4_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for %%P in (vswhere.exe) do if not defined FM4_VSWHERE set "FM4_VSWHERE=%%~$PATH:P"
if not defined FM4_VSWHERE goto fm4_msvc_missing
for /f "usebackq delims=" %%I in (`"%FM4_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FM4_VS_ROOT=%%I"
if not exist "%FM4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" goto fm4_msvc_missing
call "%FM4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "FM4_VSWHERE="
set "FM4_VS_ROOT="
exit /b 0
:fm4_msvc_existing
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat"
exit /b %errorlevel%
:fm4_msvc_missing
echo A Visual Studio C++ x64 toolchain was not found. Install C++ Build Tools or use an x64 Native Tools Command Prompt.
exit /b 1
