@echo off
setlocal
wsl.exe --cd "%~dp0." --exec bash ./run-server.sh %*
exit /b %errorlevel%
