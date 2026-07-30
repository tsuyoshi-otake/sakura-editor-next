@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..\..\..") do set "REPOSITORY_ROOT=%%~fI"
pushd "%REPOSITORY_ROOT%" || exit /b 2
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Measure-ExtensionHostPerformance.ps1" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
