@echo off
setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0..\..\..") do set "REPOSITORY_ROOT=%%~fI"
set "VERIFY_SCRIPT=%~dp0verify-extension-host-processes.ps1"
set "ACCEPTANCE_SCRIPT=%~dp0Run-OpenVsxAcceptance.ps1"
set "RESULT=0"

pushd "%REPOSITORY_ROOT%" || exit /b 2
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%VERIFY_SCRIPT%" -RepositoryRoot "%REPOSITORY_ROOT%" -Platform x64 -Configuration Debug
if errorlevel 1 (
    echo Extension-host test processes already exist. Refusing to mix acceptance runs. 1>&2
    set "RESULT=2"
    goto :cleanup
)

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ACCEPTANCE_SCRIPT%"
if errorlevel 1 set "RESULT=!errorlevel!"

:cleanup
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%VERIFY_SCRIPT%" -RepositoryRoot "%REPOSITORY_ROOT%" -Platform x64 -Configuration Debug -Cleanup
if errorlevel 1 (
    echo Failed to clean Open VSX acceptance processes. 1>&2
    if "!RESULT!"=="0" set "RESULT=3"
)

popd
exit /b %RESULT%
