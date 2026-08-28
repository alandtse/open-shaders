@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0BuildFushrodah.ps1" %*
set "exit_code=%ERRORLEVEL%"
endlocal & exit /b %exit_code%
