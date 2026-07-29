@echo off
setlocal

set "platform=%~1"
set "configuration=%~2"

if "%platform%" == "Win32" (
	@rem OK
) else if "%platform%" == "x64" (
	@rem OK
) else (
	call :showhelp "%~f0"
	endlocal & exit /b 1
)

if "%configuration%" == "Release" (
	@rem OK
) else if "%configuration%" == "Debug" (
	@rem OK
) else (
	call :showhelp "%~f0"
	endlocal & exit /b 1
)

if not defined CMD_MSBUILD call "%~dp0tools\find-tools.bat"
if not defined CMD_MSBUILD (
	echo msbuild.exe was not found.
	endlocal & exit /b 1
)

set "PROJECT_FILE=%~dp0sakura_core\sakura.vcxproj"
set "MSBUILDDISABLENODEREUSE=1"
set "BUILD_TARGET=Build"
if defined SAKURA_DEV_BUILD_TARGET set "BUILD_TARGET=%SAKURA_DEV_BUILD_TARGET%"

@echo "%CMD_MSBUILD%" "%PROJECT_FILE%" /p:Platform=%platform% /p:Configuration=%configuration% /t:"%BUILD_TARGET%" /nr:false
      "%CMD_MSBUILD%" "%PROJECT_FILE%" /p:Platform=%platform% /p:Configuration=%configuration% /t:"%BUILD_TARGET%" /nr:false

if errorlevel 1 goto :builderror

endlocal & exit /b 0

:builderror
set "build_exit=%errorlevel%"
echo ERROR in msbuild.exe errorlevel %build_exit%
endlocal & exit /b %build_exit%

@rem ------------------------------------------------------------------------------
:showhelp
@echo off
@echo usage
@echo    %~nx1 platform configuration
@echo.
@echo parameter
@echo    platform      : Win32   or x64
@echo    configuration : Release or Debug
@echo.
@echo example
@echo    %~nx1 Win32 Release
@echo    %~nx1 Win32 Debug
@echo    %~nx1 x64   Release
@echo    %~nx1 x64   Debug
exit /b 0
