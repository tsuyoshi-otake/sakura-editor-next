@echo off
setlocal
set "platform=%~1"
set "configuration=%~2"

if "%platform%" == "x64" (
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

set "SLN_FILE=%~dp0sakura.sln"

set "EXTRA_CMD="
set "LOG_DIR=%~dp0build\logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
if errorlevel 1 (
	echo Failed to create MSBuild log directory: %LOG_DIR%
	endlocal & exit /b 1
)
set "LOG_FILE=%LOG_DIR%\msbuild-%platform%-%configuration%.log"
@rem https://msdn.microsoft.com/ja-jp/library/ms171470.aspx
set "LOG_OPTION=/flp:logfile=%LOG_FILE%"
set "MSBUILDDISABLENODEREUSE=1"

	@echo "%CMD_MSBUILD%" "%SLN_FILE%" /p:Platform=%platform% /p:Configuration=%configuration% /t:"Build" /nr:false %EXTRA_CMD% %LOG_OPTION%
	      "%CMD_MSBUILD%" "%SLN_FILE%" /p:Platform=%platform% /p:Configuration=%configuration% /t:"Build" /nr:false %EXTRA_CMD% %LOG_OPTION%

if errorlevel 1 goto :builderror

endlocal & exit /b 0

:builderror
set "build_exit=%errorlevel%"
echo ERROR in msbuild.exe errorlevel %build_exit%
endlocal & exit /b %build_exit%

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
