@echo off
setlocal
wsl.exe --cd "%~dp0." --exec bash ./run-client.sh %*
exit /b %errorlevel%
