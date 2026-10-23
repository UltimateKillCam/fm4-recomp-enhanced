@echo off
setlocal
set "FM4_GAME_ROOT=%~dp0extracted"
if exist "%~dp0.local\game-disc1\default.xex" set "FM4_GAME_ROOT=%~dp0.local\game-disc1"
if exist "%~dp0.local\disc2-content\" (
  call "%~dp0tools\Run-PowerShell.cmd" -NoProfile -File "%~dp0tools\Run-Local.ps1" -GameDataRoot "%FM4_GAME_ROOT%" -Disc2ContentRoot "%~dp0.local\disc2-content"
) else (
  call "%~dp0tools\Run-PowerShell.cmd" -NoProfile -File "%~dp0tools\Run-Local.ps1" -GameDataRoot "%FM4_GAME_ROOT%"
)
if errorlevel 1 pause
