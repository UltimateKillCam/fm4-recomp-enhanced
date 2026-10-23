@echo off
setlocal
for %%I in ("%~dp0..") do set "FM4_ROOT=%%~fI"
call "%~dp0Use-MSVC.cmd"
if errorlevel 1 exit /b 1
set "PATH=%FM4_ROOT%\.local\tools\llvm\bin;%PATH%"
set "FM4_CLANG="
set "FM4_CLANGXX="
set "FM4_NINJA="
for /f "delims=" %%I in ('where clang.exe 2^>nul') do if not defined FM4_CLANG set "FM4_CLANG=%%I"
for /f "delims=" %%I in ('where clang++.exe 2^>nul') do if not defined FM4_CLANGXX set "FM4_CLANGXX=%%I"
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined FM4_NINJA set "FM4_NINJA=%%I"
if not defined FM4_CLANG (
  echo Clang was not found on PATH.
  exit /b 1
)
if not defined FM4_CLANGXX (
  echo Clang++ was not found on PATH.
  exit /b 1
)
if not defined FM4_NINJA (
  echo Ninja was not found on PATH.
  exit /b 1
)
"%FM4_CLANGXX%" -dumpmachine | findstr /C:"windows-msvc" >nul
if errorlevel 1 (
  echo Clang must target the Windows MSVC ABI.
  exit /b 1
)
call "%~dp0Run-PowerShell.cmd" -NoProfile -ExecutionPolicy Bypass -File "%FM4_ROOT%\tools\Prepare-Enhanced-Sdk.ps1"
if errorlevel 1 exit /b 1
python "%FM4_ROOT%\tools\Prepare-Resolve-Shaders.py"
if errorlevel 1 exit /b 1
for %%B in (8 16 32 64 128) do (
  "%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe" /D SHADING_LANGUAGE_HLSL_XE=1 /T cs_5_1 /E main /O3 /Vn resolve_full_%%Bbpp_scaled_cs /Fh "%FM4_ROOT%\.local\rexglue-enhanced-sdk\src\graphics\shaders\bytecode\d3d12_5_1\resolve_full_%%Bbpp_scaled_cs.h" "%FM4_ROOT%\.local\resolve-shaders\src\xenia\gpu\shaders\resolve_full_%%Bbpp_scaled.cs.xesl"
  if errorlevel 1 exit /b 1
)
"%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe" /T cs_5_1 /E main /O3 /Vn resolve_downscale_cs /Fh "%FM4_ROOT%\.local\rexglue-enhanced-sdk\src\graphics\shaders\bytecode\d3d12_5_1\resolve_downscale_cs.h" "%FM4_ROOT%\tools\graphics\resolve_downscale.cs.hlsl"
if errorlevel 1 exit /b 1
copy /y "%FM4_ROOT%\tools\graphics\resolve_downscale.cs.hlsl" "%FM4_ROOT%\.local\rexglue-enhanced-sdk\src\graphics\shaders\resolve_downscale.cs.hlsl" >nul
cmake -S "%FM4_ROOT%\.local\rexglue-enhanced-sdk" -B "%FM4_ROOT%\.local\sdk-enhanced-build" -G "Ninja Multi-Config" -DCMAKE_C_COMPILER="%FM4_CLANG%" -DCMAKE_CXX_COMPILER="%FM4_CLANGXX%" -DCMAKE_MAKE_PROGRAM="%FM4_NINJA%" -DCMAKE_CXX_STANDARD=23 -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DCMAKE_CONFIGURATION_TYPES=Release -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_OUTPUT_DIRECTORY="%FM4_ROOT%/.local/sdk-enhanced-out" -DCMAKE_INSTALL_PREFIX="%FM4_ROOT%/.local/sdk-enhanced-install" -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="%FM4_ROOT%/tools/sdk/Extend-Sdk.cmake" -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
if errorlevel 1 exit /b 1
cmake --build "%FM4_ROOT%\.local\sdk-enhanced-build" --config Release --target install --parallel 3
exit /b %errorlevel%
