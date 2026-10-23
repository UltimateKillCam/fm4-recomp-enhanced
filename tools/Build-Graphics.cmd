@echo off
setlocal
for %%I in ("%~dp0..") do set "FM4_ROOT=%%~fI"
call "%~dp0Use-MSVC.cmd"
if errorlevel 1 exit /b 1
set "PATH=%FM4_ROOT%\.local\tools\llvm\bin;%PATH%"
set "FM4_CLANGXX="
set "FM4_NINJA="
for /f "delims=" %%I in ('where clang++.exe 2^>nul') do if not defined FM4_CLANGXX set "FM4_CLANGXX=%%I"
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined FM4_NINJA set "FM4_NINJA=%%I"
if not defined FM4_CLANGXX (
  echo Clang++ was not found on PATH.
  exit /b 1
)
if not defined FM4_NINJA (
  echo Ninja was not found on PATH.
  exit /b 1
)
"%FM4_CLANGXX%" -dumpmachine | findstr /C:"windows-msvc" >nul
if errorlevel 1 exit /b 1
cmake -S "%FM4_ROOT%\tools\graphics" -B "%FM4_ROOT%\.local\graphics-build" -G Ninja -DCMAKE_CXX_COMPILER="%FM4_CLANGXX%" -DCMAKE_MAKE_PROGRAM="%FM4_NINJA%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "%FM4_ROOT%\.local\graphics-build" --parallel 2
if errorlevel 1 exit /b 1
ctest --test-dir "%FM4_ROOT%\.local\graphics-build" --output-on-failure
exit /b %errorlevel%
