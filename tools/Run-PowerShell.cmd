@echo off
rem Launcher dependency resolution. Modified 2026-10-17. GPL-3.0-only.
setlocal
set "FM4_SHELL="
if defined FM4_POWERSHELL if exist "%FM4_POWERSHELL%" set "FM4_SHELL=%FM4_POWERSHELL%"
if not defined FM4_SHELL if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "FM4_SHELL=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not defined FM4_SHELL if exist "%ProgramW6432%\PowerShell\7\pwsh.exe" set "FM4_SHELL=%ProgramW6432%\PowerShell\7\pwsh.exe"
if not defined FM4_SHELL if exist "%LOCALAPPDATA%\Programs\PowerShell\7\pwsh.exe" set "FM4_SHELL=%LOCALAPPDATA%\Programs\PowerShell\7\pwsh.exe"
if not defined FM4_SHELL if exist "%LOCALAPPDATA%\Microsoft\PowerShell\7\pwsh.exe" set "FM4_SHELL=%LOCALAPPDATA%\Microsoft\PowerShell\7\pwsh.exe"
for %%P in (pwsh.exe) do if not defined FM4_SHELL set "FM4_SHELL=%%~$PATH:P"
if not defined FM4_SHELL if exist "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" set "FM4_SHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not defined FM4_SHELL if exist "%~dp0..\.local\tools\powershell\pwsh.exe" set "FM4_SHELL=%~dp0..\.local\tools\powershell\pwsh.exe"
if not defined FM4_SHELL (
  echo Windows PowerShell 5.1 or PowerShell 7 is required but neither could be located.
  echo A custom installation can be selected with the FM4_POWERSHELL environment variable.
  exit /b 9009
)
rem A parent PowerShell 7 process may have exported its module search path.
rem Prefer the selected host's own modules while retaining custom module paths.
for %%P in ("%FM4_SHELL%") do set "PSModulePath=%%~dpPModules;%PSModulePath%"
"%FM4_SHELL%" %*
exit /b %errorlevel%
