@echo off
setlocal
py -3 "%~dp0tools\build\sakura_build.py" compat build-gnu %*
set "sakura_build_exit=%errorlevel%"
endlocal & exit /b %sakura_build_exit%
