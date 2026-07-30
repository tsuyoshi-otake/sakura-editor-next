@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PLATFORM=%~1"
if not defined PLATFORM set "PLATFORM=x64"
set "CONFIGURATION=%~2"
if not defined CONFIGURATION set "CONFIGURATION=Debug"
for %%I in ("%~dp0..\..\..") do set "REPOSITORY_ROOT=%%~fI"
set "VERIFY_SCRIPT=%~dp0verify-extension-host-processes.ps1"
set "TEST_EXE=%REPOSITORY_ROOT%\%PLATFORM%\%CONFIGURATION%\tests1.exe"
set "RESULT=0"

pushd "%REPOSITORY_ROOT%" || exit /b 2

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%VERIFY_SCRIPT%" -RepositoryRoot "%REPOSITORY_ROOT%" -Platform "%PLATFORM%" -Configuration "%CONFIGURATION%"
if errorlevel 1 (
    echo Extension-host test processes already exist. Refusing to mix test runs. 1>&2
    set "RESULT=2"
    goto :cleanup
)

call npm --prefix src\exthost test
if errorlevel 1 (
    set "RESULT=!errorlevel!"
    goto :cleanup
)

call npm --prefix src\exthost run build
if errorlevel 1 (
    set "RESULT=!errorlevel!"
    goto :cleanup
)

if not exist "%TEST_EXE%" (
    echo Missing %TEST_EXE%. Run build-sln.bat %PLATFORM% %CONFIGURATION% first. 1>&2
    set "RESULT=2"
    goto :cleanup
)

"%TEST_EXE%" --gtest_filter=CExtension*.*:CJsonRpcFrameCodec.*
if errorlevel 1 set "RESULT=!errorlevel!"

:cleanup
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%VERIFY_SCRIPT%" -RepositoryRoot "%REPOSITORY_ROOT%" -Platform "%PLATFORM%" -Configuration "%CONFIGURATION%" -Cleanup
if errorlevel 1 (
    echo Failed to clean extension-host test processes. 1>&2
    if "!RESULT!"=="0" set "RESULT=3"
)

popd
exit /b %RESULT%
