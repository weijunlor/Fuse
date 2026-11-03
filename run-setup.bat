@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-setup.ps1" %*
exit /b %errorlevel%
