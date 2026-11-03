@echo off
setlocal
wsl.exe --cd "%~dp0." --exec bash run-server.sh --interface eth3 %*
exit /b %errorlevel%
