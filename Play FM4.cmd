@echo off
setlocal
cd /d "%~dp0"
call "%~dp0tools\Run-PowerShell.cmd" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Run-Local.ps1" -GameDataRoot "%~dp0.local\game-disc1" -Disc2ContentRoot "%~dp0.local\disc2-content" %*
set "FM4_LAUNCH_EXIT=%errorlevel%"
if not "%FM4_LAUNCH_EXIT%"=="0" pause
exit /b %FM4_LAUNCH_EXIT%
