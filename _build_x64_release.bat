@echo off
setlocal
set "PATH=C:\Program Files\7-Zip;%PATH%"
rem Allow scripts in the current directory to run (Onigmo/bregonig build_nmake.cmd)
set "NoDefaultCurrentDirectoryInExePath="
rem Do not leave MSBuild worker nodes alive: they keep holding intermediate
rem files (e.g. *.asm) and make the next build fail with "Permission denied".
set "MSBUILDDISABLENODEREUSE=1"
set "SKIP_CREATE_GITHASH=1"
cd /d "%~dp0"

rem A failed incremental-LTCG link leaves link.exe alive holding the *.asm
rem files that /FAsu writes, which makes the *next* build fail the same way.
rem Break that loop before starting: kill survivors, drop the listings.
echo --- clean up leftovers from a previous build ---
taskkill /F /IM link.exe    >nul 2>&1
taskkill /F /IM cl.exe      >nul 2>&1
taskkill /F /IM MSBuild.exe >nul 2>&1
del /Q "build\x64\Release\sakura_core\*.asm" >nul 2>&1
del /Q "build\x64\Release\tests1\*.asm"      >nul 2>&1

echo --- where 7z ---
where 7z
echo --- start build ---
call .\build-sln.bat x64 Release
echo BUILD_EXIT=%errorlevel%
endlocal
