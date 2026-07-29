@echo off
set platform=%1
set configuration=%2
set ISS_LOG_FILE=iss-%platform%-%configuration%.log

if "%platform%" == "x64" (
	@rem OK
) else (
	call :showhelp %0
	exit /b 1
)

if "%configuration%" == "Release" (
	@rem OK
) else if "%configuration%" == "Debug" (
	@rem OK
) else (
	call :showhelp %0
	exit /b 1
)

if not defined CMD_ISCC call %~dp0tools\find-tools.bat
if not defined CMD_7Z call %~dp0tools\find-tools.bat
if not defined CMD_ISCC (
	echo ISCC.exe was not found.
	exit /b 1
)
if not defined CMD_7Z (
	echo 7z.exe was not found. It is required to stage installer dependencies.
	exit /b 1
)

set INSTALLER_WORK=installer\sakura
set INSTALLER_OUTPUT=installer\Output-%platform%

set INSTALLER_RESOURCES_SINT=installer\sinst_src
set INSTALLER_RESOURCES_BRON=installer\temp\bron
set INSTALLER_RESOURCES_CTAGS=installer\temp\ctags

if exist "%INSTALLER_WORK%"      rmdir /s /q "%INSTALLER_WORK%"
if exist "%INSTALLER_OUTPUT%"    rmdir /s /q "%INSTALLER_OUTPUT%"

mkdir %INSTALLER_WORK%
mkdir %INSTALLER_WORK%\license\bregonig
mkdir %INSTALLER_WORK%\keyword
mkdir %INSTALLER_WORK%\license\ctags\
mkdir %INSTALLER_WORK%\license\windows-terminal\
mkdir %INSTALLER_WORK%\license\fmt\
mkdir %INSTALLER_WORK%\license\ms-gsl\
mkdir %INSTALLER_WORK%\license\wil\

set BREGONIG_DLL=bregonig.dll
set BRON_ZIP=installer\externals\bregonig\bron420.zip
"%CMD_7Z%" e "%BRON_ZIP%" -o"%INSTALLER_RESOURCES_BRON%" -y *.txt || (echo error extracting bregonig license files && exit /b 1)
"%CMD_7Z%" e "%BRON_ZIP%" -o"%platform%\%configuration%" -y x64/%BREGONIG_DLL% || (echo error extracting %BREGONIG_DLL% && exit /b 1)

set CTAGS_EXE=ctags.exe
set CTAGS_PREFIX=x64
set CTAGS_ZIP=installer\externals\universal-ctags\ctags-v6.1.0-%CTAGS_PREFIX%.zip
"%CMD_7Z%" x "%CTAGS_ZIP%" -o"%INSTALLER_RESOURCES_CTAGS%" -y license || (echo error extracting ctags license files && exit /b 1)
"%CMD_7Z%" e "%CTAGS_ZIP%" -o"%platform%\%configuration%" -y %CTAGS_EXE% || (echo error extracting %CTAGS_EXE% && exit /b 1)

set WINDOWS_TERMINAL_VENDOR=%~dp0sakura_core\terminal\vendor\windows_terminal
set WINDOWS_TERMINAL_LICENSES=%~dp0sakura_core\terminal\vendor\licenses
if not exist "%WINDOWS_TERMINAL_VENDOR%\LICENSE" (
	echo Windows Terminal license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_VENDOR%\UPSTREAM.md" (
	echo Windows Terminal provenance payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_VENDOR%\IMPORTED_FILES.md" (
	echo Windows Terminal imported-files payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_LICENSES%\fmt\LICENSE" (
	echo fmt license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_LICENSES%\ms-gsl\LICENSE" (
	echo Microsoft GSL license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_LICENSES%\wil\LICENSE" (
	echo WIL license payload was not found.
	exit /b 1
)

copy /Y .\LICENSE                                           %INSTALLER_WORK%\license\ > NUL
copy /Y %INSTALLER_RESOURCES_SINT%\sakura.exe.manifest.x    %INSTALLER_WORK%\ > NUL
copy /Y %INSTALLER_RESOURCES_SINT%\sakura.exe.manifest.v    %INSTALLER_WORK%\ > NUL
copy /Y %INSTALLER_RESOURCES_SINT%\sakura.exe.ini           %INSTALLER_WORK%\ > NUL
copy /Y %INSTALLER_RESOURCES_SINT%\keyword\*.*              %INSTALLER_WORK%\keyword\ > NUL
copy /Y %INSTALLER_RESOURCES_BRON%\*.txt                    %INSTALLER_WORK%\license\bregonig\ > NUL
copy /Y %INSTALLER_RESOURCES_CTAGS%\license\*.*             %INSTALLER_WORK%\license\ctags\ > NUL
copy /Y %WINDOWS_TERMINAL_VENDOR%\LICENSE                   %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_VENDOR%\UPSTREAM.md               %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal provenance && exit /b 1)
copy /Y %WINDOWS_TERMINAL_VENDOR%\IMPORTED_FILES.md         %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal imported-files list && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\fmt\LICENSE             %INSTALLER_WORK%\license\fmt\ > NUL || (echo error copying fmt license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\ms-gsl\LICENSE          %INSTALLER_WORK%\license\ms-gsl\ > NUL || (echo error copying Microsoft GSL license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\wil\LICENSE             %INSTALLER_WORK%\license\wil\ > NUL || (echo error copying WIL license && exit /b 1)

copy /Y /B help\sakura\sakura.chm                           %INSTALLER_WORK%\ > NUL
copy /Y /B help\plugin\plugin.chm                           %INSTALLER_WORK%\ > NUL
copy /Y /B help\macro\macro.chm                             %INSTALLER_WORK%\ > NUL

copy /Y /B %platform%\%configuration%\*.exe                 %INSTALLER_WORK%\ > NUL
copy /Y /B %platform%\%configuration%\*.dll                 %INSTALLER_WORK%\ > NUL

set SAKURA_ISS=installer\sakura-%platform%.iss
@echo running "%CMD_ISCC%" %SAKURA_ISS%
"%CMD_ISCC%" %SAKURA_ISS% > %ISS_LOG_FILE% || (echo error && exit /b 1)
exit /b 0

@rem ------------------------------------------------------------------------------
@rem show help
@rem see http://orangeclover.hatenablog.com/entry/20101004/1286120668
@rem ------------------------------------------------------------------------------
:showhelp
@echo off
@echo usage
@echo    %~nx1 platform configuration
@echo.
@echo parameter
@echo    platform      : x64
@echo    configuration : Release or Debug
@echo.
@echo example
@echo    %~nx1 x64   Release
@echo    %~nx1 x64   Debug
exit /b 0
