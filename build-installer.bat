@echo off
@setlocal EnableExtensions
set "SAKURA_UTF16_PRODUCTION_PACKAGE=true"
if not defined SAKURA_UTF16_BACKEND set "SAKURA_UTF16_BACKEND=rust"
if not "%SAKURA_UTF16_BACKEND%" == "rust" (
	echo Production packaging requires SAKURA_UTF16_BACKEND=rust; got %SAKURA_UTF16_BACKEND%.
	exit /b 1
)
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
set INSTALLER_RESOURCES_CTAGS=installer\temp\ctags
set BREGONIG_LICENSE_DIR=third_party\owned\bregonig-next

if exist "%INSTALLER_WORK%"      rmdir /s /q "%INSTALLER_WORK%"
if exist "%INSTALLER_OUTPUT%"    rmdir /s /q "%INSTALLER_OUTPUT%"

mkdir %INSTALLER_WORK%
mkdir %INSTALLER_WORK%\license\bregonig
mkdir %INSTALLER_WORK%\keyword
mkdir %INSTALLER_WORK%\license\ctags\
mkdir %INSTALLER_WORK%\license\windows-terminal\
mkdir %INSTALLER_WORK%\license\codicons\
mkdir %INSTALLER_WORK%\license\seti\
mkdir %INSTALLER_WORK%\license\fmt\
mkdir %INSTALLER_WORK%\license\ms-gsl\
mkdir %INSTALLER_WORK%\license\wil\

set BREGONIG_DLL=bregonig.dll
set MIGEMO_DLL=migemo.dll
set SENP_TOOL_EXE=sakura-senp-tool.exe
set SENP_HOST_EXE=sakura-senp-host.exe
if not exist "%platform%\%configuration%\%BREGONIG_DLL%" (
	echo error: %platform%\%configuration%\%BREGONIG_DLL% was not staged by the product build.
	echo The installer must ship that DLL. It must not extract installer\externals\bregonig\bron420.zip.
	exit /b 1
)
if not exist "%platform%\%configuration%\%MIGEMO_DLL%" (
	echo error: %platform%\%configuration%\%MIGEMO_DLL% was not staged by the product build.
	exit /b 1
)
if not exist "%platform%\%configuration%\%SENP_TOOL_EXE%" (
	echo error: %platform%\%configuration%\%SENP_TOOL_EXE% was not staged by the product build.
	exit /b 1
)
if not exist "%platform%\%configuration%\%SENP_HOST_EXE%" (
	echo error: %platform%\%configuration%\%SENP_HOST_EXE% was not staged by the product build.
	exit /b 1
)
if not exist "%BREGONIG_LICENSE_DIR%\bsd_license.txt" (
	echo error: bregonig license files were not found under %BREGONIG_LICENSE_DIR%.
	exit /b 1
)

set CTAGS_EXE=ctags.exe
set CTAGS_PREFIX=x64
set CTAGS_ZIP=installer\externals\universal-ctags\ctags-v6.1.0-%CTAGS_PREFIX%.zip
"%CMD_7Z%" x "%CTAGS_ZIP%" -o"%INSTALLER_RESOURCES_CTAGS%" -y license || (echo error extracting ctags license files && exit /b 1)
"%CMD_7Z%" e "%CTAGS_ZIP%" -o"%platform%\%configuration%" -y %CTAGS_EXE% || (echo error extracting %CTAGS_EXE% && exit /b 1)

set WINDOWS_TERMINAL_VENDOR=%~dp0sakura_core\terminal\vendor\windows_terminal
set WINDOWS_TERMINAL_LICENSES=%~dp0sakura_core\terminal\vendor\licenses
set CODICONS_VENDOR=%~dp0sakura_core\workbench\icons
if not exist "%CODICONS_VENDOR%\CODICONS-ATTRIBUTION.md" (
	echo Codicons attribution payload was not found.
	exit /b 1
)
set SETI_VENDOR=%~dp0sakura_core\workbench\icons
if not exist "%SETI_VENDOR%\SETI-ATTRIBUTION.md" (
	echo Seti attribution payload was not found.
	exit /b 1
)
if not exist "%SETI_VENDOR%\SETI-LICENSE" (
	echo Seti license payload was not found.
	exit /b 1
)
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
copy /Y /B %BREGONIG_LICENSE_DIR%\bsd_license.txt           %INSTALLER_WORK%\license\bregonig\ > NUL || (echo error copying bregonig bsd license && exit /b 1)
copy /Y /B %BREGONIG_LICENSE_DIR%\perl_license.txt          %INSTALLER_WORK%\license\bregonig\ > NUL || (echo error copying bregonig perl license && exit /b 1)
copy /Y /B %BREGONIG_LICENSE_DIR%\perl_license_jp.txt       %INSTALLER_WORK%\license\bregonig\ > NUL || (echo error copying bregonig perl license ja && exit /b 1)
copy /Y %INSTALLER_RESOURCES_CTAGS%\license\*.*             %INSTALLER_WORK%\license\ctags\ > NUL
copy /Y %WINDOWS_TERMINAL_VENDOR%\LICENSE                   %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_VENDOR%\UPSTREAM.md               %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal provenance && exit /b 1)
copy /Y %WINDOWS_TERMINAL_VENDOR%\IMPORTED_FILES.md         %INSTALLER_WORK%\license\windows-terminal\ > NUL || (echo error copying Windows Terminal imported-files list && exit /b 1)
copy /Y %CODICONS_VENDOR%\CODICONS-ATTRIBUTION.md           %INSTALLER_WORK%\license\codicons\ > NUL || (echo error copying Codicons attribution && exit /b 1)
copy /Y %SETI_VENDOR%\SETI-ATTRIBUTION.md                   %INSTALLER_WORK%\license\seti\ > NUL || (echo error copying Seti attribution && exit /b 1)
copy /Y %SETI_VENDOR%\SETI-LICENSE                          %INSTALLER_WORK%\license\seti\ > NUL || (echo error copying Seti license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\fmt\LICENSE             %INSTALLER_WORK%\license\fmt\ > NUL || (echo error copying fmt license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\ms-gsl\LICENSE          %INSTALLER_WORK%\license\ms-gsl\ > NUL || (echo error copying Microsoft GSL license && exit /b 1)
copy /Y %WINDOWS_TERMINAL_LICENSES%\wil\LICENSE             %INSTALLER_WORK%\license\wil\ > NUL || (echo error copying WIL license && exit /b 1)

copy /Y /B help\sakura\sakura.chm                           %INSTALLER_WORK%\ > NUL
copy /Y /B help\plugin\plugin.chm                           %INSTALLER_WORK%\ > NUL
copy /Y /B help\macro\macro.chm                             %INSTALLER_WORK%\ > NUL

copy /Y /B %platform%\%configuration%\*.exe                 %INSTALLER_WORK%\ > NUL
copy /Y /B %platform%\%configuration%\*.dll                 %INSTALLER_WORK%\ > NUL

if not exist "build\logs" mkdir build\logs
py -3 "%~dp0tools\verify_runtime_artifact_identity.py" --staged "%platform%\%configuration%" --installer-work "%INSTALLER_WORK%" --clean-extract "installer\temp\runtime-identity-%platform%-%configuration%-work" --report "build\logs\runtime-artifact-identity-%platform%-%configuration%-installer-work.json" || (echo error: staged runtime files do not match the installer work directory && exit /b 1)

set SAKURA_ISS=installer\sakura-%platform%.iss
@echo running "%CMD_ISCC%" %SAKURA_ISS%
if "%configuration%" == "Release" (
	"%CMD_ISCC%" /DREQUIRE_AVX=1 %SAKURA_ISS% > %ISS_LOG_FILE% || (echo error && exit /b 1)
) else (
	"%CMD_ISCC%" %SAKURA_ISS% > %ISS_LOG_FILE% || (echo error && exit /b 1)
)

set INSTALLER_EXE=
for %%I in ("%INSTALLER_OUTPUT%\*.exe") do set INSTALLER_EXE=%%~fI
if not defined INSTALLER_EXE (
	echo error: Inno Setup did not produce an installer exe under %INSTALLER_OUTPUT%.
	exit /b 1
)
py -3 "%~dp0tools\verify_runtime_artifact_identity.py" --staged "%platform%\%configuration%" --installer-exe "%INSTALLER_EXE%" --seven-zip "%CMD_7Z%" --clean-extract "installer\temp\runtime-identity-%platform%-%configuration%-exe" --report "build\logs\runtime-artifact-identity-%platform%-%configuration%-installer.json" || (echo error: staged runtime files do not match the installer payload && exit /b 1)
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
