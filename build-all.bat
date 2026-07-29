@echo off
setlocal
set platform=%1
set configuration=%2

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

@echo PLATFORM      %PLATFORM%
@echo CONFIGURATION %CONFIGURATION%
@echo.

@echo ---- start build-sln.bat ----
set SAKURA_GENERATE_ASSEMBLY_LISTINGS=1
call build-sln.bat       %PLATFORM% %CONFIGURATION% || (echo error build-sln.bat       && exit /b 1)
@echo ---- end   build-sln.bat ----
@echo.

@echo ---- start build-chm.bat ----
call build-chm.bat                                  || (echo error build-chm.bat       && exit /b 1)
@echo ---- end   build-chm.bat ----
@echo.

@echo ---- start build-installer.bat ----
call build-installer.bat %PLATFORM% %CONFIGURATION% || (echo error build-installer.bat && exit /b 1)
@echo ---- end   build-installer.bat ----
@echo.

@echo ---- start zipArtifacts.bat ----
call zipArtifacts.bat    %PLATFORM% %CONFIGURATION% || (echo error zipArtifacts.bat    && exit /b 1)
@echo ---- end   zipArtifacts.bat ----
@echo.

endlocal & exit /b 0

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
