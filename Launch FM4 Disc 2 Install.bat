@echo off
setlocal

if "%~1"=="" (
  echo Usage: %~nx0 ^<folder-containing-extracted-Disc-2-packages^>
  exit /b 2
)

set "FM4_DISC2_ROOT=%~f1"
if not exist "%FM4_DISC2_ROOT%\" (
  echo Disc 2 package folder not found: %FM4_DISC2_ROOT%
  exit /b 2
)

set "FM4_GAME_ROOT=%~dp0extracted"
if exist "%~dp0.local\game-disc1\default.xex" set "FM4_GAME_ROOT=%~dp0.local\game-disc1"
call "%~dp0tools\Run-PowerShell.cmd" -NoProfile -File "%~dp0tools\Run-Local.ps1" -GameDataRoot "%FM4_GAME_ROOT%" -Disc2ContentRoot "%FM4_DISC2_ROOT%"
