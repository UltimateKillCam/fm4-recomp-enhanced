@echo off
setlocal
cd /d "%~dp0"
call "%~dp0tools\Run-PowerShell.cmd" -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\Configure-Enhanced.ps1" %*
set "FM4_LAUNCH_EXIT=%errorlevel%"
if not "%FM4_LAUNCH_EXIT%"=="0" pause
exit /b %FM4_LAUNCH_EXIT%
