@echo off
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\main\ps1\check-encoding.ps1" %*
exit /b %ERRORLEVEL%
